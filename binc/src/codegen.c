/* codegen.c — emit AIR (LLVM IR) for BinC.
 * Model: alloca-based locals + basic-block control flow (clang -O0 style; mem2reg cleans up).
 * Type-directed: int (i32) vs float (f32) vs bool (i1). Oracle: ../proof/blend_vision_proof.ll.
 */
#include "binc.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

typedef struct { char *p; size_t n, cap; } SB;
static void sb_put(SB *s, const char *str){ size_t l=strlen(str);
    if(s->n+l+1>s->cap){ s->cap=s->cap?s->cap*2:256; while(s->n+l+1>s->cap)s->cap*=2; s->p=realloc(s->p,s->cap); }
    memcpy(s->p+s->n,str,l); s->n+=l; s->p[s->n]='\0'; }
static void sb_printf(SB *s, const char *fmt, ...){ va_list a; va_start(a,fmt);
    char buf[2048]; vsnprintf(buf,sizeof buf,fmt,a); va_end(a); sb_put(s,buf); }

static StructDef *find_struct(const Program *p, const char *t){
    for(size_t i=0;i<p->nstructs;i++) if(!strcmp(p->structs[i].tag,t)) return &p->structs[i]; return NULL; }
static const char *scalar_ll(TypeKind k){
    switch(k){ case T_FLOAT: return "float"; case T_HALF: return "half"; case T_BOOL: return "i1";
        case T_INT32: case T_UINT32: return "i32"; default: return "float"; } }
static ValKind scalar_vk(TypeKind k){
    switch(k){ case T_FLOAT: case T_HALF: return VK_F32; case T_BOOL: return VK_I1;
        case T_INT32: return VK_I32; case T_UINT32: return VK_U32; default: return VK_F32; } }
static int size_of(TypeKind k){ return k==T_HALF?2:k==T_BOOL?1:4; }
static int align_of(TypeKind k){ return k==T_HALF?2:k==T_BOOL?1:4; }
static const char *type_name(TypeKind k){
    switch(k){ case T_FLOAT: return "float"; case T_HALF: return "half"; case T_BOOL: return "bool";
        case T_INT32: return "int"; case T_UINT32: return "uint"; default: return "float"; } }
/* MSL vector layout (probed): vec2 = 2*elem size/align, vec3/vec4 = 4*elem size/align */
static int type_size(TypeKind k,int vecn){ int s=size_of(k); return vecn==2?s*2:vecn>2?s*4:s; }
static int type_align(TypeKind k,int vecn){ int a=align_of(k); return vecn==2?a*2:vecn>2?a*4:a; }
static void ll_of(char *buf,size_t n,TypeKind k,int vecn){
    if(vecn>1) snprintf(buf,n,"<%d x %s>",vecn,scalar_ll(k)); else snprintf(buf,n,"%s",scalar_ll(k)); }
static void tn_of(char *buf,size_t n,TypeKind k,int vecn){
    if(vecn>1) snprintf(buf,n,"%s%d",type_name(k),vecn); else snprintf(buf,n,"%s",type_name(k)); }
/* real struct size (tail-padded) and alignment per the AIR datalayout */
static int struct_layout(StructDef *s, int *al){
    int off=0,m=1;
    for(size_t i=0;i<s->nfields;i++){ int fa=type_align(s->fields[i].ty.kind,s->fields[i].ty.vecn);
        off=(off+fa-1)&~(fa-1); off+=type_size(s->fields[i].ty.kind,s->fields[i].ty.vecn); if(fa>m)m=fa; }
    *al=m; return (off+m-1)&~(m-1);
}

typedef struct { char *name; char *slot; TypeKind kind; char *sname; int vecn; } Loc;
typedef struct {
    SB *pre,*body; const Program *prog; Function *fn;
    int tmp; char *idx; int *read,*written; char **scalar_load;
    Loc *locs; size_t nlocs;
    int term; /* current block terminated */
    int lblc; /* label counter (separate from tmp) */
    int brk_l[32], cont_l[32], nloops; /* break/continue label stack */
    int uses_sync; /* body contains a barrier -> function must be convergent, not nosync */
    int rvw; /* vector width of the value gen_rval just returned (0 = scalar) */
} CG;
static char *newtmp(CG *c){ char *s=malloc(16); snprintf(s,16,"%%t%d",c->tmp++); return s; }
static int newlbl(CG *c){ return c->lblc++; }
static void emit(CG *c,const char *fmt,...){ va_list a; va_start(a,fmt); char b[1024]; vsnprintf(b,sizeof b,fmt,a); va_end(a); sb_put(c->body,b); }
static void lbl(CG *c,int n){ char b[32]; snprintf(b,sizeof b,"bb%d:\n",n); sb_put(c->body,b); c->term=0; }
/* implicit numeric coercion (int/uint<->float/bool) for assignments/inits */
static const char *coerce(CG *c, const char *v, ValKind from, ValKind to){
    if(from==to) return v;
    if(from==VK_I32 && to==VK_U32) return v; /* same i32 bits, signedness is a BinC-level fact */
    if(from==VK_U32 && to==VK_I32) return v;
    if(from==VK_I32 && to==VK_F32){ const char *r=newtmp(c); emit(c,"  %s = sitofp i32 %s to float\n",r,v); return r; }
    if(from==VK_U32 && to==VK_F32){ const char *r=newtmp(c); emit(c,"  %s = uitofp i32 %s to float\n",r,v); return r; }
    if(from==VK_F32 && to==VK_I32){ const char *r=newtmp(c); emit(c,"  %s = fptosi float %s to i32\n",r,v); return r; }
    if(from==VK_F32 && to==VK_U32){ const char *r=newtmp(c); emit(c,"  %s = fptoui float %s to i32\n",r,v); return r; }
    if(from==VK_I1 && to==VK_F32){ const char *z=newtmp(c); emit(c,"  %s = zext i1 %s to i32\n",z,v); const char *r=newtmp(c); emit(c,"  %s = sitofp i32 %s to float\n",r,z); return r; }
    if(from==VK_I1 && (to==VK_I32||to==VK_U32)){ const char *r=newtmp(c); emit(c,"  %s = zext i1 %s to i32\n",r,v); return r; }
    return v;
}

/* resolve a name to: local | pointer param idx | scalar param idx */
typedef enum { R_NONE,R_LOCAL,R_PTR,R_SCALAR } RKind;
static RKind resolve(CG *c, const char *name, int *out){
    for(size_t i=0;i<c->nlocs;i++) if(!strcmp(c->locs[i].name,name)){ *out=(int)i; return R_LOCAL; }
    for(size_t i=0;i<c->fn->nparams;i++) if(!strcmp(c->fn->params[i].name,name)){
        *out=(int)i; return c->fn->params[i].ty.is_ptr?R_PTR:R_SCALAR; }
    return R_NONE;
}
static int root_param(CG *c, Expr *e){
    while(e&&(e->kind==E_DEREF||e->kind==E_FIELD||e->kind==E_INDEX)) e=e->operand;
    if(e&&e->kind==E_IDENT){ int idx; if(resolve(c,e->name,&idx)==R_PTR) return idx; } return -1; }

/* lvalue description: element type + where the address lives */
typedef struct { TypeKind tk; char *sname; int as; int pi; int is_local; int vecn; } LInfo;
static void pty_str(char *buf,size_t n,TypeKind tk,char *sname,int as,int is_local,int vecn){
    char elt[64];
    if(tk==T_STRUCT) snprintf(elt,sizeof elt,"%%struct.%s",sname); else ll_of(elt,sizeof elt,tk,vecn);
    if(is_local) snprintf(buf,n,"%s*",elt); else snprintf(buf,n,"%s addrspace(%d)*",elt,as); }

/* address of element p[ix] (i64 index value) for pointer param pi */
static char *element_ptr_idx(CG *c,int pi,const char *ix){
    Param *pr=&c->fn->params[pi]; char elt[64];
    if(pr->ty.kind==T_STRUCT) snprintf(elt,sizeof elt,"%%struct.%s",pr->ty.struct_name);
    else ll_of(elt,sizeof elt,pr->ty.kind,pr->ty.vecn);
    char *s=newtmp(c);
    emit(c,"  %s = getelementptr inbounds %s, %s addrspace(%d)* %%_%s, i64 %s\n",s,elt,elt,pr->ty.as,pr->name,ix);
    return s;
}
static char *gen_lval(CG *c, Expr *e, LInfo *li, int mark);

static const char *gen_rval(CG *c, Expr *e, ValKind *k);

/* pointer expression -> owning param + i64 index value; bare `p` means the implicit thread index */
static int eval_ptr(CG *c, Expr *e, const char **ix){
    if(e->kind==E_IDENT){ int i; if(resolve(c,e->name,&i)==R_PTR){
        if(!c->fn->is_kernel) die(0,"implicit *%s needs a kernel thread id; subscript explicitly: %s[i]",e->name,e->name);
        *ix=c->idx; return i; } }
    if(e->kind==E_BIN&&(e->bop==B_ADD||e->bop==B_SUB)){
        const char *base; int pi=eval_ptr(c,e->lhs,&base);
        ValKind rk; const char *off=gen_rval(c,e->rhs,&rk);
        if(rk!=VK_I32&&rk!=VK_U32) die(0,"pointer offset must be an integer");
        const char *o64=newtmp(c); emit(c,"  %s = sext i32 %s to i64\n",o64,off);
        char *r=newtmp(c); emit(c,"  %s = %s i64 %s, %s\n",r,e->bop==B_ADD?"add":"sub",base,o64);
        *ix=r; return pi; }
    die(0,"unsupported pointer expression");
}
static void fill_param_li(CG *c,int pi,LInfo *li){
    Param *pr=&c->fn->params[pi];
    li->tk=pr->ty.kind; li->sname=pr->ty.struct_name; li->as=pr->ty.as; li->pi=pi; li->is_local=0; li->vecn=pr->ty.vecn; }

/* load an lvalue, promoting half to float; *k gets the expression-level kind, c->rvw the vector width */
static const char *emit_load_t(CG *c, LInfo *li, const char *addr, ValKind *k){
    if(li->tk==T_STRUCT) die(0,"cannot use a whole struct as a value");
    char pty[96]; pty_str(pty,sizeof pty,li->tk,li->sname,li->as,li->is_local,li->vecn);
    char ll[32]; ll_of(ll,sizeof ll,li->tk,li->vecn); const char *v=newtmp(c);
    emit(c,"  %s = load %s, %s %s, align %d\n",v,ll,pty,addr,type_align(li->tk,li->vecn));
    c->rvw=li->vecn>1?li->vecn:0;
    if(li->tk==T_HALF){ const char *w=newtmp(c); emit(c,"  %s = fpext half %s to float\n",w,v); v=w; }
    *k=scalar_vk(li->tk); return v;
}
/* convert an rvalue for storage into type tk (half demotes via fptrunc); *exprv = pre-demotion value */
static const char *store_val(CG *c, const char *v, ValKind from, TypeKind tk, const char **exprv){
    v=coerce(c,v,from,scalar_vk(tk)); *exprv=v;
    if(tk==T_HALF){ const char *w=newtmp(c); emit(c,"  %s = fptrunc float %s to half\n",w,v); return w; }
    return v;
}
/* splat a scalar into an <n x elt> vector */
static const char *splat(CG *c, const char *v, const char *elt, int n){
    const char *acc="undef";
    for(int i=0;i<n;i++){ const char *r=newtmp(c);
        emit(c,"  %s = insertelement <%d x %s> %s, %s %s, i32 %d\n",r,n,elt,acc,elt,v,i); acc=r; }
    return acc;
}
/* element conversion between <n x i32> and <n x float> */
static const char *vconv(CG *c, const char *v, int n, ValKind from, ValKind to){
    const char *op=to==VK_F32?(from==VK_U32?"uitofp":"sitofp"):(to==VK_U32?"fptoui":"fptosi");
    const char *r=newtmp(c);
    emit(c,"  %s = %s <%d x %s> %s to <%d x %s>\n",r,op,n,from==VK_F32?"float":"i32",v,n,to==VK_F32?"float":"i32");
    return r;
}
/* prepare an rvalue (kind from, width from_vw) for storage into (tk, vecn) */
static const char *to_storage(CG *c, const char *v, ValKind from, int from_vw, TypeKind tk, int vecn){
    if(vecn>1){ if(from_vw&&from_vw!=vecn) die(0,"vector width mismatch");
        ValKind want=scalar_vk(tk);
        if(!from_vw){ v=coerce(c,v,from,want); return splat(c,v,scalar_ll(tk),vecn); }
        if(from!=want) return vconv(c,v,vecn,from,want);
        return v; }
    if(from_vw) die(0,"cannot store a vector into a scalar");
    const char *ev; return store_val(c,v,from,tk,&ev);
}
/* is `name` a vector type constructor (float2..uint4)? */
static int vec_name(const char *s,TypeKind *k,int *n){
    size_t l=strlen(s); if(l<2) return 0; char w=s[l-1]; if(w<'2'||w>'4') return 0;
    char base[8]; if(l-1>=sizeof base) return 0; memcpy(base,s,l-1); base[l-1]=0;
    if(!strcmp(base,"float"))*k=T_FLOAT; else if(!strcmp(base,"int"))*k=T_INT32;
    else if(!strcmp(base,"uint"))*k=T_UINT32; else return 0;
    *n=w-'0'; return 1;
}
/* component name -> index, or -1 */
static int comp_idx(const char *f){
    const char *names="xyzw"; const char *rgba="rgba";
    const char *p=strchr(names,f[0]); if(!p) p=strchr(rgba,f[0]);
    if(!p||f[1]) return -1; return (int)(p-(p==strchr(names,f[0])?names:rgba))%4;
}

/* builtins: AIR spellings probed from the metal frontend (metal -emit-llvm -S on MSL using
 * metal::sqrt etc.; the module sets air.compile.fast_math_enable, hence the fast_ variants).
 * sync() is the threadgroup barrier: air.wg.barrier(i32 2, i32 5, i32 1). */
typedef struct { const char *name,*ll; int nargs; TypeKind ret, a0, a1; } Builtin;
static Builtin builtins[]={
    {"sqrt","air.fast_sqrt.f32",1,T_FLOAT,T_FLOAT,T_FLOAT},
    {"fabs","air.fast_fabs.f32",1,T_FLOAT,T_FLOAT,T_FLOAT},
    {"floor","air.fast_floor.f32",1,T_FLOAT,T_FLOAT,T_FLOAT},
    {"ceil","air.fast_ceil.f32",1,T_FLOAT,T_FLOAT,T_FLOAT},
    {"sin","air.fast_sin.f32",1,T_FLOAT,T_FLOAT,T_FLOAT},
    {"cos","air.fast_cos.f32",1,T_FLOAT,T_FLOAT,T_FLOAT},
    {"exp","air.fast_exp.f32",1,T_FLOAT,T_FLOAT,T_FLOAT},
    {"log","air.fast_log.f32",1,T_FLOAT,T_FLOAT,T_FLOAT},
    {"fmin","air.fast_fmin.f32",2,T_FLOAT,T_FLOAT,T_FLOAT},
    {"fmax","air.fast_fmax.f32",2,T_FLOAT,T_FLOAT,T_FLOAT},
    {"pow","air.fast_pow.f32",2,T_FLOAT,T_FLOAT,T_FLOAT},
    {"imin","air.min.s.i32",2,T_INT32,T_INT32,T_INT32},
    {"imax","air.max.s.i32",2,T_INT32,T_INT32,T_INT32},
    {"sync","air.wg.barrier",0,T_VOID,T_VOID,T_VOID},
};
static int builtin_used[sizeof builtins/sizeof *builtins];

static const char *cmp_name(CmpOp op,int isfloat,int isuns){
    if(isfloat) switch(op){ case C_EQ:return "oeq"; case C_NE:return "one"; case C_LT:return "olt";
        case C_LE:return "ole"; case C_GT:return "ogt"; case C_GE:return "oge"; }
    if(isuns) switch(op){ case C_EQ:return "eq"; case C_NE:return "ne"; case C_LT:return "ult";
        case C_LE:return "ule"; case C_GT:return "ugt"; case C_GE:return "uge"; }
    switch(op){ case C_EQ:return "eq"; case C_NE:return "ne"; case C_LT:return "slt";
        case C_LE:return "sle"; case C_GT:return "sgt"; case C_GE:return "sge"; } return "eq";
}
static const char *as_op(AssignOp a, int isfloat, int isuns){
    if(isfloat) switch(a){ case A_ADDEQ:return "fadd"; case A_SUBEQ:return "fsub"; case A_MULEQ:return "fmul";
        case A_DIVEQ:return "fdiv"; case A_MODEQ:return "frem"; default:return NULL; }
    switch(a){ case A_ADDEQ:return "add"; case A_SUBEQ:return "sub"; case A_MULEQ:return "mul";
        case A_DIVEQ:return isuns?"udiv":"sdiv"; case A_MODEQ:return isuns?"urem":"srem"; default:return NULL; }
}

static const char *gen_rval(CG *c, Expr *e, ValKind *k){
    c->rvw=0; /* cases set this to the vector width of their result, if any */
    switch(e->kind){
    case E_FCONST:{ *k=VK_F32; char *s=malloc(32); snprintf(s,32,"%e",e->fval); return s; }
    case E_ICONST:{ *k=VK_I32; char *s=malloc(16); snprintf(s,16,"%ld",e->ival); return s; }
    case E_BOOL:{ *k=VK_I1; return e->bval?"true":"false"; }
    case E_IDENT:{
        int idx; RKind r=resolve(c,e->name,&idx);
        if(r==R_LOCAL){ LInfo li={c->locs[idx].kind,c->locs[idx].sname,0,-1,1,c->locs[idx].vecn};
            return emit_load_t(c,&li,c->locs[idx].slot,k); }
        if(r==R_SCALAR){ Param *p=&c->fn->params[idx]; *k=scalar_vk(p->ty.kind); c->rvw=p->ty.vecn>1?p->ty.vecn:0;
            if(!c->scalar_load[idx]){
                if(c->fn->is_kernel){ char ll[32]; ll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn); const char *v=newtmp(c);
                    emit(c,"  %s = load %s, %s addrspace(2)* %%_%s, align %d\n",v,ll,ll,p->name,type_align(p->ty.kind,p->ty.vecn));
                    if(p->ty.kind==T_HALF){ const char *w=newtmp(c); emit(c,"  %s = fpext half %s to float\n",w,v); v=w; }
                    c->scalar_load[idx]=(char*)v;
                } else { char *nm=malloc(strlen(p->name)+3); snprintf(nm,strlen(p->name)+3,"%%_%s",p->name);
                    if(p->ty.kind==T_HALF){ const char *w=newtmp(c); emit(c,"  %s = fpext half %s to float\n",w,nm); nm=(char*)w; }
                    c->scalar_load[idx]=nm; } }
            return c->scalar_load[idx]; }
        die(0,"undefined name %s",e->name);
    }
    case E_DEREF: case E_FIELD: case E_INDEX:{
        LInfo li; char *addr=gen_lval(c,e,&li,0);
        if(li.pi>=0) c->read[li.pi]=1;
        return emit_load_t(c,&li,addr,k);
    }
    case E_NEG:{ ValKind lk; const char *v=gen_rval(c,e->operand,&lk); int vw=c->rvw; const char *r=newtmp(c);
        char ty[32]; ll_of(ty,sizeof ty,lk==VK_F32?T_FLOAT:T_INT32,vw);
        if(lk==VK_F32){ *k=VK_F32; emit(c,"  %s = fneg fast %s %s\n",r,ty,v); }
        else { *k=lk; emit(c,"  %s = sub %s %s, %s\n",r,ty,vw?"zeroinitializer":"0",v); }
        c->rvw=vw; return r; }
    case E_NOT:{ ValKind lk; const char *v=gen_rval(c,e->operand,&lk); if(lk!=VK_I1) die(0,"! on non-bool");
        const char *r=newtmp(c); *k=VK_I1; emit(c,"  %s = xor i1 %s, true\n",r,v); return r; }
    case E_BIN:{ ValKind lk,rk; const char *l=gen_rval(c,e->lhs,&lk); int lw=c->rvw;
        const char *r=gen_rval(c,e->rhs,&rk); int rw=c->rvw;
        int isf=(lk==VK_F32||rk==VK_F32);
        int uns=(lk==VK_U32||rk==VK_U32);
        ValKind ok=isf?VK_F32:uns?VK_U32:VK_I32;
        int vw=lw?lw:rw;
        if(lw&&rw&&lw!=rw) die(0,"vector width mismatch");
        if(vw){ const char *elt=ok==VK_F32?"float":"i32";
            if(!lw){ l=coerce(c,l,lk,ok); l=splat(c,l,elt,vw); } else if(lk!=ok) l=vconv(c,l,lw,lk,ok);
            if(!rw){ r=coerce(c,r,rk,ok); r=splat(c,r,elt,vw); } else if(rk!=ok) r=vconv(c,r,rw,rk,ok);
            const char *op; if(isf) op=e->bop==B_ADD?"fadd fast":e->bop==B_SUB?"fsub fast":e->bop==B_MUL?"fmul fast":e->bop==B_DIV?"fdiv fast":"frem fast";
            else op=e->bop==B_ADD?"add":e->bop==B_SUB?"sub":e->bop==B_MUL?"mul":e->bop==B_DIV?(uns?"udiv":"sdiv"):(uns?"urem":"srem");
            const char *v=newtmp(c); *k=ok; c->rvw=vw;
            emit(c,"  %s = %s <%d x %s> %s, %s\n",v,op,vw,elt,l,r); return v; }
        if(isf){ l=coerce(c,l,lk,VK_F32); r=coerce(c,r,rk,VK_F32); }
        const char *op; if(isf) op=e->bop==B_ADD?"fadd fast":e->bop==B_SUB?"fsub fast":e->bop==B_MUL?"fmul fast":e->bop==B_DIV?"fdiv fast":"frem fast";
        else op=e->bop==B_ADD?"add":e->bop==B_SUB?"sub":e->bop==B_MUL?"mul":e->bop==B_DIV?(uns?"udiv":"sdiv"):(uns?"urem":"srem");
        const char *v=newtmp(c); *k=ok;
        emit(c,"  %s = %s %s %s, %s\n",v,op,isf?"float":"i32",l,r); return v; }
    case E_CMP:{ ValKind lk,rk; const char *l=gen_rval(c,e->lhs,&lk); int lw=c->rvw;
        const char *r=gen_rval(c,e->rhs,&rk);
        if(lw||c->rvw) die(0,"vector comparison not supported");
        int isf=(lk==VK_F32||rk==VK_F32);
        if(isf){ l=coerce(c,l,lk,VK_F32); r=coerce(c,r,rk,VK_F32); }
        int uns=(lk==VK_U32||rk==VK_U32);
        const char *v=newtmp(c); *k=VK_I1;
        emit(c,"  %s = %s %s %s %s, %s\n",v,isf?"fcmp":"icmp",cmp_name(e->cmp,isf,uns),isf?"float":"i32",l,r); return v; }
    case E_LOG:{ ValKind lk,rk; const char *l=gen_rval(c,e->lhs,&lk); int lw=c->rvw;
        const char *r=gen_rval(c,e->rhs,&rk); if(lw||c->rvw) die(0,"vector operand in logical operator");
        const char *v=newtmp(c); *k=VK_I1; emit(c,"  %s = %s i1 %s, %s\n",v,e->log==L_AND?"and":"or",l,r); return v; }
    case E_ASSIGN:{
        ValKind rk; const char *rhs=gen_rval(c,e->rhs,&rk); int rw=c->rvw;
        LInfo li; char *addr=gen_lval(c,e->operand,&li,1);
        if(li.tk==T_STRUCT) die(0,"cannot assign a whole struct");
        int vw=li.vecn;
        if(e->aop!=A_ASSIGN){
            ValKind ck; const char *cur=emit_load_t(c,&li,addr,&ck);
            if(li.pi>=0) c->read[li.pi]=1;
            int isf=(ck==VK_F32||rk==VK_F32);
            int uns=!isf&&(ck==VK_U32||rk==VK_U32);
            ValKind ok=isf?VK_F32:uns?VK_U32:VK_I32;
            const char *nv=newtmp(c);
            if(vw){ const char *elt=ok==VK_F32?"float":"i32";
                if(rw&&rw!=vw) die(0,"vector width mismatch");
                if(ck!=ok) cur=vconv(c,cur,vw,ck,ok);
                if(!rw){ rhs=coerce(c,rhs,rk,ok); rhs=splat(c,rhs,elt,vw); } else if(rk!=ok) rhs=vconv(c,rhs,rw,rk,ok);
                emit(c,"  %s = %s <%d x %s> %s, %s\n",nv,as_op(e->aop,isf,uns),vw,elt,cur,rhs); rw=vw;
            } else { if(isf){ cur=coerce(c,cur,ck,VK_F32); rhs=coerce(c,rhs,rk,VK_F32); }
                emit(c,"  %s = %s %s %s, %s\n",nv,as_op(e->aop,isf,uns),isf?"float":"i32",cur,rhs); }
            rhs=nv; rk=ok;
        }
        const char *sv, *ev;
        if(vw){ sv=to_storage(c,rhs,rk,rw,li.tk,vw); ev=sv; }
        else { if(rw) die(0,"cannot store a vector into a scalar"); sv=store_val(c,rhs,rk,li.tk,&ev); }
        char pty[96]; pty_str(pty,sizeof pty,li.tk,li.sname,li.as,li.is_local,vw);
        char ll[32]; ll_of(ll,sizeof ll,li.tk,vw);
        emit(c,"  store %s %s, %s %s, align %d\n",ll,sv,pty,addr,type_align(li.tk,vw));
        *k=scalar_vk(li.tk); c->rvw=vw; return ev;
    }
    case E_CALL:{
        /* vector constructor? float4(...)/int3(...)/uint2(...), incl. single-scalar splat */
        TypeKind cb; int cn;
        if(vec_name(e->name,&cb,&cn)){
            if(e->nargs!=1&&(int)e->nargs!=cn) die(0,"%s expects 1 or %d argument(s)",e->name,cn);
            const char *elt=scalar_ll(cb); const char *acc="undef";
            for(int i=0;i<cn;i++){
                ValKind ak; const char *v=gen_rval(c,e->args[e->nargs==1?0:i],&ak);
                if(c->rvw) die(0,"vector argument in %s constructor",e->name);
                v=coerce(c,v,ak,scalar_vk(cb));
                const char *r=newtmp(c);
                emit(c,"  %s = insertelement <%d x %s> %s, %s %s, i32 %d\n",r,cn,elt,acc,elt,v,i); acc=r; }
            *k=scalar_vk(cb); c->rvw=cn; return acc;
        }
        /* user function? the whole Program is visible, so definition order doesn't matter */
        Function *f=NULL; for(size_t i=0;i<c->prog->nfuncs;i++)
            if(!strcmp(c->prog->funcs[i].name,e->name)){ f=&c->prog->funcs[i]; break; }
        if(f){
            if(f->is_kernel) die(0,"cannot call kernel function %s",e->name);
            if(f==c->fn) die(0,"recursion is not supported on the GPU (%s calls itself)",e->name);
            if(e->nargs!=f->nparams) die(0,"%s expects %d argument(s), got %d",e->name,(int)f->nparams,(int)e->nargs);
            char args[2048]; size_t o=0;
            for(size_t i=0;i<f->nparams;i++){ Param *p=&f->params[i];
                if(i)o+=snprintf(args+o,sizeof args-o,", ");
                if(p->ty.is_ptr){ Expr *a=e->args[i]; int pi;
                    if(a->kind!=E_IDENT||resolve(c,a->name,&pi)!=R_PTR)
                        die(0,"%s: pointer argument %d must be one of the caller's buffer parameters",e->name,(int)i+1);
                    Param *cp=&c->fn->params[pi];
                    if(cp->ty.kind!=p->ty.kind||cp->ty.as!=p->ty.as||cp->ty.vecn!=p->ty.vecn)
                        die(0,"%s: pointer argument %d type/address-space mismatch",e->name,(int)i+1);
                    char elt[64]; if(p->ty.kind==T_STRUCT)snprintf(elt,sizeof elt,"%%struct.%s",p->ty.struct_name);
                        else ll_of(elt,sizeof elt,p->ty.kind,p->ty.vecn);
                    o+=snprintf(args+o,sizeof args-o,"%s addrspace(%d)* %%_%s",elt,p->ty.as,cp->name);
                } else { ValKind ak; const char *v=gen_rval(c,e->args[i],&ak);
                    const char *sv=to_storage(c,v,ak,c->rvw,p->ty.kind,p->ty.vecn);
                    char ll[32]; ll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn);
                    o+=snprintf(args+o,sizeof args-o,"%s %s",ll,sv); }
            }
            if(f->ret.kind==T_VOID){ emit(c,"  call void @%s(%s)\n",f->name,args); *k=VK_I32; c->rvw=0; return "0"; }
            const char *r=newtmp(c); char rll[32]; ll_of(rll,sizeof rll,f->ret.kind,f->ret.vecn);
            emit(c,"  %s = call %s @%s(%s)\n",r,rll,f->name,args);
            if(f->ret.kind==T_HALF){ const char *w=newtmp(c); emit(c,"  %s = fpext half %s to float\n",w,r); r=w; }
            *k=scalar_vk(f->ret.kind); c->rvw=f->ret.vecn>1?f->ret.vecn:0; return r;
        }
        for(size_t b=0;b<sizeof builtins/sizeof *builtins;b++){ Builtin *bi=&builtins[b];
            if(strcmp(bi->name,e->name)) continue;
            if(e->nargs!=(size_t)bi->nargs) die(0,"%s expects %d argument(s), got %d",e->name,bi->nargs,(int)e->nargs);
            builtin_used[b]=1;
            if(bi->ret==T_VOID){ c->uses_sync=1; emit(c,"  call void @%s(i32 2, i32 5, i32 1)\n",bi->ll); *k=VK_I32; return "0"; }
            char args[256]; size_t o=0;
            for(int i=0;i<bi->nargs;i++){ ValKind ak; const char *v=gen_rval(c,e->args[i],&ak);
                if(c->rvw) die(0,"vector argument to builtin %s",e->name);
                TypeKind at=i==0?bi->a0:bi->a1; const char *ev; const char *sv=store_val(c,v,ak,at,&ev);
                o+=snprintf(args+o,sizeof args-o,"%s%s %s",i?", ":"",scalar_ll(at),sv); }
            const char *r=newtmp(c); *k=scalar_vk(bi->ret);
            emit(c,"  %s = call %s @%s(%s)\n",r,scalar_ll(bi->ret),bi->ll,args); return r;
        }
        die(0,"undefined function %s",e->name);
    }
    }
    die(0,"unreachable");
}
static char *gen_lval(CG *c, Expr *e, LInfo *li, int mark){
    if(e->kind==E_IDENT){
        int idx; RKind r=resolve(c,e->name,&idx);
        if(r==R_LOCAL){ li->tk=c->locs[idx].kind; li->sname=c->locs[idx].sname; li->as=0; li->pi=-1;
            li->is_local=1; li->vecn=c->locs[idx].vecn; return c->locs[idx].slot; }
        die(0,"%s is not a mutable local",e->name);
    }
    if(e->kind==E_DEREF){ const char *ix; int pi=eval_ptr(c,e->operand,&ix);
        if(mark)c->written[pi]=1; fill_param_li(c,pi,li); return element_ptr_idx(c,pi,ix); }
    if(e->kind==E_INDEX){
        int pi; const char *base=NULL;
        if(e->operand->kind==E_IDENT){ if(resolve(c,e->operand->name,&pi)!=R_PTR) die(0,"subscript of non-pointer"); }
        else pi=eval_ptr(c,e->operand,&base); /* e.g. (p+1)[i] */
        ValKind ik; const char *iv=gen_rval(c,e->rhs,&ik);
        if(ik!=VK_I32&&ik!=VK_U32) die(0,"subscript must be an integer");
        const char *i64=newtmp(c); emit(c,"  %s = sext i32 %s to i64\n",i64,iv);
        if(base){ char *s=newtmp(c); emit(c,"  %s = add i64 %s, %s\n",s,base,i64); i64=s; }
        if(mark)c->written[pi]=1; fill_param_li(c,pi,li); return element_ptr_idx(c,pi,i64);
    }
    if(e->kind==E_FIELD){ char *addr=gen_lval(c,e->operand,li,mark);
        if(li->tk==T_STRUCT){
            StructDef *s=find_struct(c->prog,li->sname); if(!s) die(0,"unknown struct %s",li->sname);
            int fi=-1; for(size_t i=0;i<s->nfields;i++) if(!strcmp(s->fields[i].name,e->field)) fi=(int)i;
            if(fi<0) die(0,"struct %s has no field %s",li->sname,e->field);
            char pty[96]; pty_str(pty,sizeof pty,T_STRUCT,li->sname,li->as,li->is_local,0);
            char *fp=newtmp(c);
            emit(c,"  %s = getelementptr inbounds %%struct.%s, %s %s, i64 0, i32 %d\n",fp,li->sname,pty,addr,fi);
            li->tk=s->fields[fi].ty.kind; li->sname=s->fields[fi].ty.struct_name; li->vecn=s->fields[fi].ty.vecn;
            return fp; }
        if(li->vecn>1){ /* vector component: bitcast the vector address to element pointer + gep */
            int ci=comp_idx(e->field);
            if(ci<0||ci>=li->vecn) die(0,"no component .%s on <%d x %s>",e->field,li->vecn,scalar_ll(li->tk));
            char vpty[96],epty[96];
            pty_str(vpty,sizeof vpty,li->tk,NULL,li->as,li->is_local,li->vecn);
            pty_str(epty,sizeof epty,li->tk,NULL,li->as,li->is_local,0);
            char *bc=newtmp(c); emit(c,"  %s = bitcast %s %s to %s\n",bc,vpty,addr,epty);
            char *cp=newtmp(c);
            emit(c,"  %s = getelementptr inbounds %s, %s %s, i64 %d\n",cp,scalar_ll(li->tk),epty,bc,ci);
            li->vecn=0; return cp; }
        die(0,"field access on non-struct");
    }
    die(0,"not an lvalue");
}
/* produce an i1 condition from any expression */
static const char *gen_cond(CG *c, Expr *e){
    ValKind k; const char *v=gen_rval(c,e,&k);
    if(c->rvw) die(0,"vector value in a condition");
    if(k==VK_I1) return v;
    const char *r=newtmp(c);
    emit(c,"  %s = %s %s %s %s, %s\n",r,k==VK_F32?"fcmp":"icmp",k==VK_F32?"one":"ne",k==VK_F32?"float":"i32",v,k==VK_F32?"0.000000e+00":"0");
    return r;
}
/* divergence heuristic: does expr touch device/constant element data (=> varying)? */
static int is_varying(CG *c, Expr *e){
    if(!e) return 0;
    if(e->kind==E_DEREF||e->kind==E_FIELD||e->kind==E_INDEX){ if(root_param(c,e)>=0) return 1; }
    return is_varying(c,e->operand)||is_varying(c,e->lhs)||is_varying(c,e->rhs);
}

static void gen_block(CG *c, Block *b);
static void gen_stmt(CG *c, Stmt *s){
    switch(s->kind){
    case S_EXPR:{ ValKind k; if(s->expr) gen_rval(c,s->expr,&k); break; }
    case S_DECL:{ TypeKind kk=s->ty.kind;
        if(kk==T_STRUCT){ StructDef *sd=find_struct(c->prog,s->ty.struct_name);
            if(!sd) die(0,"unknown struct %s",s->ty.struct_name);
            if(s->init) die(0,"struct local initializer not supported; assign fields instead");
            int sal; struct_layout(sd,&sal);
            char *slot=newtmp(c); sb_printf(c->pre,"  %s = alloca %%struct.%s, align %d\n",slot,s->ty.struct_name,sal);
            c->locs=realloc(c->locs,(c->nlocs+1)*sizeof(Loc));
            c->locs[c->nlocs++]=(Loc){s->name,slot,kk,s->ty.struct_name,0};
            break; }
        char ll[32]; ll_of(ll,sizeof ll,kk,s->ty.vecn);
        char *slot=newtmp(c); sb_printf(c->pre,"  %s = alloca %s, align %d\n",slot,ll,type_align(kk,s->ty.vecn));
        c->locs=realloc(c->locs,(c->nlocs+1)*sizeof(Loc));
        c->locs[c->nlocs++]=(Loc){s->name,slot,kk,NULL,s->ty.vecn};
        if(s->init){ ValKind k; const char *v=gen_rval(c,s->init,&k);
            const char *sv=to_storage(c,v,k,c->rvw,kk,s->ty.vecn);
            emit(c,"  store %s %s, %s* %s, align %d\n",ll,sv,ll,slot,type_align(kk,s->ty.vecn)); }
        break; }
    case S_RETURN:{ TypeKind rk=c->fn->ret.kind;
        if(s->expr){ if(rk==T_VOID) die(0,"return with a value in void function");
            ValKind k; const char *v=gen_rval(c,s->expr,&k);
            const char *sv=to_storage(c,v,k,c->rvw,rk,c->fn->ret.vecn);
            char ll[32]; ll_of(ll,sizeof ll,rk,c->fn->ret.vecn);
            emit(c,"  ret %s %s\n",ll,sv); }
        else { if(rk!=T_VOID) die(0,"return without a value in non-void function");
            emit(c,"  ret void\n"); }
        c->term=1; break; }
    case S_BREAK:{ if(!c->nloops) die(0,"break outside a loop");
        emit(c,"  br label %%bb%d\n",c->brk_l[c->nloops-1]); c->term=1; break; }
    case S_CONTINUE:{ if(!c->nloops) die(0,"continue outside a loop");
        emit(c,"  br label %%bb%d\n",c->cont_l[c->nloops-1]); c->term=1; break; }
    case S_BLOCK:{ gen_block(c,&s->then_b); break; }
    case S_IF:{
        if(is_varying(c,s->cond)) fprintf(stderr,"binc: note: 'if' condition is data-dependent (divergent branch) — may cost SIMT performance\n");
        const char *cv=gen_cond(c,s->cond);
        int tl=newlbl(c), fl=newlbl(c), en=newlbl(c);
        int has_else=s->else_b.n>0;
        emit(c,"  br i1 %s, label %%bb%d, label %%bb%d\n",cv,tl,has_else?fl:en);
        lbl(c,tl); gen_block(c,&s->then_b); if(!c->term) emit(c,"  br label %%bb%d\n",en);
        if(has_else){ lbl(c,fl); gen_block(c,&s->else_b); if(!c->term) emit(c,"  br label %%bb%d\n",en); }
        lbl(c,en); break; }
    case S_WHILE:{
        if(is_varying(c,s->cond)) fprintf(stderr,"binc: note: 'while' condition is data-dependent — divergent\n");
        int cond=newlbl(c), body=newlbl(c), en=newlbl(c);
        emit(c,"  br label %%bb%d\n",cond); lbl(c,cond);
        const char *cv=gen_cond(c,s->cond); emit(c,"  br i1 %s, label %%bb%d, label %%bb%d\n",cv,body,en);
        lbl(c,body);
        c->brk_l[c->nloops]=en; c->cont_l[c->nloops]=cond; c->nloops++;
        gen_block(c,&s->then_b); c->nloops--;
        if(!c->term) emit(c,"  br label %%bb%d\n",cond); lbl(c,en); break; }
    case S_FOR:{
        if(s->for_init) gen_stmt(c,s->for_init);
        int cond=newlbl(c), body=newlbl(c), inc=newlbl(c), en=newlbl(c);
        emit(c,"  br label %%bb%d\n",cond); lbl(c,cond);
        if(s->for_cond){ if(is_varying(c,s->for_cond)) fprintf(stderr,"binc: note: 'for' bound is data-dependent (varying) — per-thread loop; consider a uniform bound\n");
            const char *cv=gen_cond(c,s->for_cond); emit(c,"  br i1 %s, label %%bb%d, label %%bb%d\n",cv,body,en); }
        else emit(c,"  br label %%bb%d\n",body);
        lbl(c,body);
        c->brk_l[c->nloops]=en; c->cont_l[c->nloops]=inc; c->nloops++;
        gen_block(c,&s->then_b); c->nloops--;
        if(!c->term) emit(c,"  br label %%bb%d\n",inc);
        lbl(c,inc); if(s->for_incr){ ValKind k; gen_rval(c,s->for_incr,&k); } if(!c->term) emit(c,"  br label %%bb%d\n",cond);
        lbl(c,en); break; }
    }
}
static void gen_block(CG *c, Block *b){ for(size_t i=0;i<b->n;i++) gen_stmt(c,&b->stmts[i]); }

static void fn_ptr_str(Function *fn,char *buf,size_t n){
    size_t o=0; o+=snprintf(buf+o,n-o,"void (");
    for(size_t i=0;i<fn->nparams;i++){ if(i)o+=snprintf(buf+o,n-o,", "); Param *p=&fn->params[i];
        if(p->ty.is_ptr){ char elt[64]; if(p->ty.kind==T_STRUCT)snprintf(elt,sizeof elt,"%%struct.%s",p->ty.struct_name);
            else ll_of(elt,sizeof elt,p->ty.kind,p->ty.vecn); o+=snprintf(buf+o,n-o,"%s addrspace(%d)*",elt,p->ty.as); }
        else o+=snprintf(buf+o,n-o,"%s addrspace(2)*",scalar_ll(p->ty.kind)); }
    if(fn->nparams)o+=snprintf(buf+o,n-o,", "); o+=snprintf(buf+o,n-o,"i32)* @%s",fn->name);
}

void emit_air(FILE *out, const Program *prog){
    memset(builtin_used,0,sizeof builtin_used);
    fprintf(out,"; generated by binc — works as C, acts as Metal\n");
    fprintf(out,"target datalayout = \"e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32\"\n");
    fprintf(out,"target triple = \"air64_v29-apple-macosx27.0.0\"\n\n");
    for(size_t i=0;i<prog->nstructs;i++){ StructDef *s=&prog->structs[i];
        fprintf(out,"%%struct.%s = type { ",s->tag);
        for(size_t j=0;j<s->nfields;j++){ if(j)fprintf(out,", "); char fl[32];
            ll_of(fl,sizeof fl,s->fields[j].ty.kind,s->fields[j].ty.vecn); fprintf(out,"%s",fl); }
        fprintf(out," }\n"); } fprintf(out,"\n");

    typedef struct { int *read,*written; int np; } KF; KF *kf=calloc(prog->nfuncs,sizeof(KF));
    for(size_t fi=0;fi<prog->nfuncs;fi++){
        Function *fn=&prog->funcs[fi]; CG c={0};
        SB pr={0},bd={0}; c.pre=&pr; c.body=&bd; c.prog=prog; c.fn=fn; c.tmp=0;
        c.read=calloc(fn->nparams,sizeof(int)); c.written=calloc(fn->nparams,sizeof(int));
        c.scalar_load=calloc(fn->nparams,sizeof(char*));
        /* signature: kernels get the hidden %_id and constant-buffer scalars;
         * non-kernels are plain internal AIR functions with by-value scalars */
        char sig[2048]; size_t so=0;
        if(fn->is_kernel) so+=snprintf(sig+so,sizeof sig-so,"define void @%s(",fn->name);
        else { char rl[32]; ll_of(rl,sizeof rl,fn->ret.kind,fn->ret.vecn);
            so+=snprintf(sig+so,sizeof sig-so,"define internal %s @%s(",rl,fn->name); }
        for(size_t i=0;i<fn->nparams;i++){ Param *p=&fn->params[i]; if(i)so+=snprintf(sig+so,sizeof sig-so,", ");
            if(p->ty.is_ptr){ char elt[64]; if(p->ty.kind==T_STRUCT)snprintf(elt,sizeof elt,"%%struct.%s",p->ty.struct_name);
                else ll_of(elt,sizeof elt,p->ty.kind,p->ty.vecn); so+=snprintf(sig+so,sizeof sig-so,"%s addrspace(%d)* nocapture noundef %%_%s",elt,p->ty.as,p->name); }
            else if(fn->is_kernel){ char ll[32]; ll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn);
                so+=snprintf(sig+so,sizeof sig-so,"%s addrspace(2)* nocapture noundef readonly align %d dereferenceable(%d) %%_%s",ll,type_align(p->ty.kind,p->ty.vecn),type_size(p->ty.kind,p->ty.vecn),p->name); }
            else { char ll[32]; ll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn);
                so+=snprintf(sig+so,sizeof sig-so,"%s noundef %%_%s",ll,p->name); } }
        if(fn->is_kernel){ if(fn->nparams)so+=snprintf(sig+so,sizeof sig-so,", ");
            so+=snprintf(sig+so,sizeof sig-so,"i32 noundef %%_id) local_unnamed_addr"); }
        else so+=snprintf(sig+so,sizeof sig-so,")");
        if(fn->is_kernel){ c.idx=malloc(16); snprintf(c.idx,16,"%%t%d",c.tmp++);
            sb_printf(c.pre,"  %s = zext i32 %%_id to i64\n",c.idx); }
        gen_block(&c,&fn->body);
        if(!c.term){ if(fn->ret.kind==T_VOID) emit(&c,"  ret void\n");
            else { char rl[32]; ll_of(rl,sizeof rl,fn->ret.kind,fn->ret.vecn);
                emit(&c,"  ret %s %s\n",rl,fn->ret.vecn>1?"zeroinitializer":
                    fn->ret.kind==T_FLOAT||fn->ret.kind==T_HALF?"0.000000e+00":fn->ret.kind==T_BOOL?"false":"0"); } }
        /* a body containing a barrier must be convergent and cannot be nosync (#1); else #0 */
        fprintf(out,"%s #%d {\n",sig,c.uses_sync?1:0);
        fwrite(pr.p,1,pr.n,out); fwrite(bd.p,1,bd.n,out);
        fprintf(out,"}\n\n");
        kf[fi].read=c.read; kf[fi].written=c.written; kf[fi].np=(int)fn->nparams;
    }
    /* declares for the builtins that were actually used */
    for(size_t b=0;b<sizeof builtins/sizeof *builtins;b++){ if(!builtin_used[b]) continue; Builtin *bi=&builtins[b];
        if(bi->ret==T_VOID){ fprintf(out,"declare void @%s(i32, i32, i32) local_unnamed_addr #3\n",bi->ll); continue; }
        char da[64]; size_t o=0;
        for(int i=0;i<bi->nargs;i++) o+=snprintf(da+o,sizeof da-o,"%s%s",i?", ":"",scalar_ll(i==0?bi->a0:bi->a1));
        fprintf(out,"declare %s @%s(%s) local_unnamed_addr #2\n",scalar_ll(bi->ret),bi->ll,da); }
    fprintf(out,"\nattributes #0 = { argmemonly mustprogress nofree norecurse nosync nounwind willreturn \"no-trapping-math\"=\"true\" }\n");
    fprintf(out,"attributes #1 = { argmemonly convergent mustprogress nofree norecurse nounwind willreturn \"no-trapping-math\"=\"true\" }\n");
    fprintf(out,"attributes #2 = { mustprogress nofree nosync nounwind readnone willreturn }\n");
    fprintf(out,"attributes #3 = { convergent mustprogress nounwind willreturn }\n\n");
    /* fully-numbered metadata — only `kernel` functions get !air.kernel entries */
    size_t nk=0; for(size_t fi=0;fi<prog->nfuncs;fi++) if(prog->funcs[fi].is_kernel) nk++;
    int next=13; int *knode=calloc(nk?nk:1,sizeof(int)),*empty=calloc(nk?nk:1,sizeof(int)),
        *arglist=calloc(nk?nk:1,sizeof(int)),**argnode=calloc(nk?nk:1,sizeof(int*)),*idnode=calloc(nk?nk:1,sizeof(int));
    size_t ki=0;
    for(size_t fi=0;fi<prog->nfuncs;fi++){ if(!prog->funcs[fi].is_kernel) continue;
        knode[ki]=next++; empty[ki]=next++; arglist[ki]=next++;
        argnode[ki]=malloc((kf[fi].np?kf[fi].np:1)*sizeof(int)); for(int a=0;a<kf[fi].np;a++)argnode[ki][a]=next++;
        idnode[ki]=next++; ki++; }
    fprintf(out,"!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6}\n");
    fprintf(out,"!air.kernel = !{"); for(size_t k=0;k<nk;k++){ if(k)fprintf(out,", "); fprintf(out,"!%d",knode[k]); } fprintf(out,"}\n");
    fprintf(out,"!air.compile_options = !{!7, !8}\n!llvm.ident = !{!9}\n!air.version = !{!10}\n!air.language_version = !{!11}\n!air.source_file_name = !{!12}\n\n");
    fprintf(out,"!0 = !{i32 2, !\"SDK Version\", [2 x i32] [i32 27, i32 0]}\n!1 = !{i32 1, !\"wchar_size\", i32 4}\n!2 = !{i32 7, !\"air.max_device_buffers\", i32 31}\n!3 = !{i32 7, !\"air.max_constant_buffers\", i32 31}\n!4 = !{i32 7, !\"air.max_threadgroup_buffers\", i32 31}\n!5 = !{i32 7, !\"air.max_textures\", i32 128}\n!6 = !{i32 7, !\"air.max_samplers\", i32 16}\n!7 = !{!\"air.compile.denorms_disable\"}\n!8 = !{!\"air.compile.fast_math_enable\"}\n!9 = !{!\"BinC compiler v0.0.1 (bootstrap, in C)\"}\n!10 = !{i32 2, i32 9, i32 0}\n!11 = !{!\"Metal\", i32 4, i32 1, i32 0}\n!12 = !{!\"binc\"}\n");
    ki=0;
    for(size_t fi=0;fi<prog->nfuncs;fi++){ Function *fn=&prog->funcs[fi]; if(!fn->is_kernel) continue;
        char fptr[2048]; fn_ptr_str(fn,fptr,sizeof fptr);
        fprintf(out,"!%d = !{}\n",empty[ki]);
        fprintf(out,"!%d = !{",arglist[ki]); for(int a=0;a<kf[fi].np;a++){ if(a)fprintf(out,", "); fprintf(out,"!%d",argnode[ki][a]); }
        if(kf[fi].np)fprintf(out,", "); fprintf(out,"!%d}\n",idnode[ki]);
        fprintf(out,"!%d = !{%s, !%d, !%d}\n",knode[ki],fptr,empty[ki],arglist[ki]);
        for(int a=0;a<kf[fi].np;a++){ Param *p=&fn->params[a];
            if(p->ty.is_ptr){ int r=kf[fi].read[a],w=kf[fi].written[a]; const char *acc=(r&&w)?"air.read_write":w?"air.write":"air.read";
                int sz=type_size(p->ty.kind,p->ty.vecn),al=type_align(p->ty.kind,p->ty.vecn); char tnb[64];
                tn_of(tnb,sizeof tnb,p->ty.kind,p->ty.vecn); const char *tn=tnb;
                if(p->ty.kind==T_STRUCT){ StructDef *s=find_struct(prog,p->ty.struct_name);
                    sz=struct_layout(s,&al); snprintf(tnb,sizeof tnb,"%s",p->ty.struct_name); tn=tnb; }
                fprintf(out,"!%d = !{i32 %d, !\"air.buffer\", !\"air.location_index\", i32 %d, i32 1, !\"%s\", !\"air.address_space\", i32 %d, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",argnode[ki][a],a,a,acc,p->ty.as,sz,al,tn,p->name); }
            else { char tnb[64]; tn_of(tnb,sizeof tnb,p->ty.kind,p->ty.vecn);
                fprintf(out,"!%d = !{i32 %d, !\"air.buffer\", !\"air.buffer_size\", i32 %d, !\"air.location_index\", i32 %d, i32 1, !\"air.read\", !\"air.address_space\", i32 2, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",argnode[ki][a],a,type_size(p->ty.kind,p->ty.vecn),a,type_size(p->ty.kind,p->ty.vecn),type_align(p->ty.kind,p->ty.vecn),tnb,p->name); } }
        fprintf(out,"!%d = !{i32 %d, !\"air.thread_position_in_grid\", !\"air.arg_type_name\", !\"uint\", !\"air.arg_name\", !\"id\"}\n",idnode[ki],(int)fn->nparams);
        ki++;
    }
}
