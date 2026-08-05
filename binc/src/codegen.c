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
    switch(k){ case T_FLOAT: case T_HALF: return "float"; case T_BOOL: return "i1";
        case T_INT32: case T_UINT32: return "i32"; default: return "float"; } }
static ValKind scalar_vk(TypeKind k){
    switch(k){ case T_FLOAT: case T_HALF: return VK_F32; case T_BOOL: return VK_I1;
        case T_INT32: case T_UINT32: return VK_I32; default: return VK_F32; } }

typedef struct { char *name; char *slot; TypeKind kind; } Loc;
typedef struct {
    SB *pre,*body; const Program *prog; Function *fn;
    int tmp; char *idx; int *read,*written; char **scalar_load;
    Loc *locs; size_t nlocs;
    int term; /* current block terminated */
    int lblc; /* label counter (separate from tmp) */
} CG;
static char *newtmp(CG *c){ char *s=malloc(16); snprintf(s,16,"%%t%d",c->tmp++); return s; }
static int newlbl(CG *c){ return c->lblc++; }
static void emit(CG *c,const char *fmt,...){ va_list a; va_start(a,fmt); char b[1024]; vsnprintf(b,sizeof b,fmt,a); va_end(a); sb_put(c->body,b); }
static void lbl(CG *c,int n){ char b[32]; snprintf(b,sizeof b,"bb%d:\n",n); sb_put(c->body,b); c->term=0; }
/* implicit numeric coercion (int<->float/bool) for assignments/inits */
static const char *coerce(CG *c, const char *v, ValKind from, ValKind to){
    if(from==to) return v;
    if(from==VK_I32 && to==VK_F32){ const char *r=newtmp(c); emit(c,"  %s = sitofp i32 %s to float\n",r,v); return r; }
    if(from==VK_F32 && to==VK_I32){ const char *r=newtmp(c); emit(c,"  %s = fptosi float %s to i32\n",r,v); return r; }
    if(from==VK_I1 && to==VK_F32){ const char *z=newtmp(c); emit(c,"  %s = zext i1 %s to i32\n",z,v); const char *r=newtmp(c); emit(c,"  %s = sitofp i32 %s to float\n",r,z); return r; }
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
static int root_param(CG *c, Expr *e){ if(e&&(e->kind==E_DEREF||e->kind==E_FIELD)) e=e->operand;
    if(e&&e->kind==E_IDENT){ int idx; if(resolve(c,e->name,&idx)==R_PTR) return idx; } return -1; }

/* address of element p[id] for pointer param pi */
static char *element_ptr(CG *c,int pi){
    Param *pr=&c->fn->params[pi]; char elt[64];
    if(pr->ty.kind==T_STRUCT) snprintf(elt,sizeof elt,"%%struct.%s",pr->ty.struct_name);
    else snprintf(elt,sizeof elt,"%s",scalar_ll(pr->ty.kind));
    char *s=newtmp(c);
    emit(c,"  %s = getelementptr inbounds %s, %s addrspace(%d)* %%_%s, i64 %s\n",s,elt,elt,pr->ty.as,pr->name,c->idx);
    return s;
}
static char *gen_lval(CG *c, Expr *e, ValKind *k, int mark);

static const char *gen_rval(CG *c, Expr *e, ValKind *k);

static const char *cmp_name(CmpOp op,int isfloat){
    if(isfloat) switch(op){ case C_EQ:return "oeq"; case C_NE:return "one"; case C_LT:return "olt";
        case C_LE:return "ole"; case C_GT:return "ogt"; case C_GE:return "oge"; }
    switch(op){ case C_EQ:return "eq"; case C_NE:return "ne"; case C_LT:return "slt";
        case C_LE:return "sle"; case C_GT:return "sgt"; case C_GE:return "sge"; } return "eq";
}
static const char *as_op(AssignOp a, int isfloat){
    if(isfloat) switch(a){ case A_ADDEQ:return "fadd"; case A_SUBEQ:return "fsub"; case A_MULEQ:return "fmul";
        case A_DIVEQ:return "fdiv"; case A_MODEQ:return "frem"; default:return NULL; }
    switch(a){ case A_ADDEQ:return "add"; case A_SUBEQ:return "sub"; case A_MULEQ:return "mul";
        case A_DIVEQ:return "sdiv"; case A_MODEQ:return "srem"; default:return NULL; }
}

static const char *gen_rval(CG *c, Expr *e, ValKind *k){
    switch(e->kind){
    case E_FCONST:{ *k=VK_F32; char *s=malloc(32); snprintf(s,32,"%e",e->fval); return s; }
    case E_ICONST:{ *k=VK_I32; char *s=malloc(16); snprintf(s,16,"%ld",e->ival); return s; }
    case E_BOOL:{ *k=VK_I1; return e->bval?"true":"false"; }
    case E_IDENT:{
        int idx; RKind r=resolve(c,e->name,&idx);
        if(r==R_LOCAL){ *k=scalar_vk(c->locs[idx].kind); const char *v=newtmp(c);
            emit(c,"  %s = load %s, %s* %s, align 4\n",v,scalar_ll(c->locs[idx].kind),scalar_ll(c->locs[idx].kind),c->locs[idx].slot); return v; }
        if(r==R_SCALAR){ Param *p=&c->fn->params[idx]; *k=scalar_vk(p->ty.kind);
            if(!c->scalar_load[idx]){ const char *v=newtmp(c);
                emit(c,"  %s = load %s, %s addrspace(2)* %%_%s, align 4\n",v,scalar_ll(p->ty.kind),scalar_ll(p->ty.kind),p->name); c->scalar_load[idx]=(char*)v; }
            return c->scalar_load[idx]; }
        die(0,"pointer %s must be dereferenced",e->name);
    }
    case E_DEREF: case E_FIELD:{
        int pi=root_param(c,e);
        if(pi<0) die(0,"dereference of non-parameter");
        if(e->kind==E_FIELD && c->fn->params[pi].ty.kind!=T_STRUCT) die(0,"-> on non-struct");
        ValKind lk; char *addr=gen_lval(c,e,&lk,0); c->read[pi]=1; *k=VK_F32;
        const char *v=newtmp(c);
        emit(c,"  %s = load float, float addrspace(%d)* %s, align 4\n",v,c->fn->params[pi].ty.as,addr); return v;
    }
    case E_NEG:{ ValKind lk; const char *v=gen_rval(c,e->operand,&lk); const char *r=newtmp(c);
        if(lk==VK_F32){ *k=VK_F32; emit(c,"  %s = fneg fast float %s\n",r,v); }
        else { *k=VK_I32; emit(c,"  %s = sub i32 0, %s\n",r,v); } return r; }
    case E_NOT:{ ValKind lk; const char *v=gen_rval(c,e->operand,&lk); if(lk!=VK_I1) die(0,"! on non-bool");
        const char *r=newtmp(c); *k=VK_I1; emit(c,"  %s = xor i1 %s, true\n",r,v); return r; }
    case E_BIN:{ ValKind lk,rk; const char *l=gen_rval(c,e->lhs,&lk); const char *r=gen_rval(c,e->rhs,&rk);
        int isf=(lk==VK_F32||rk==VK_F32);
        if(isf && lk==VK_I32){ const char *x=newtmp(c); emit(c,"  %s = sitofp i32 %s to float\n",x,l); l=x; }
        if(isf && rk==VK_I32){ const char *x=newtmp(c); emit(c,"  %s = sitofp i32 %s to float\n",x,r); r=x; }
        const char *op; if(isf) op=e->bop==B_ADD?"fadd fast":e->bop==B_SUB?"fsub fast":e->bop==B_MUL?"fmul fast":e->bop==B_DIV?"fdiv fast":"frem fast";
        else op=e->bop==B_ADD?"add":e->bop==B_SUB?"sub":e->bop==B_MUL?"mul":e->bop==B_DIV?"sdiv":"srem";
        const char *v=newtmp(c); *k=isf?VK_F32:VK_I32;
        emit(c,"  %s = %s %s %s, %s\n",v,op,isf?"float":"i32",l,r); return v; }
    case E_CMP:{ ValKind lk,rk; const char *l=gen_rval(c,e->lhs,&lk); const char *r=gen_rval(c,e->rhs,&rk);
        int isf=(lk==VK_F32||rk==VK_F32);
        if(isf && lk==VK_I32){ const char *x=newtmp(c); emit(c,"  %s = sitofp i32 %s to float\n",x,l); l=x; }
        if(isf && rk==VK_I32){ const char *x=newtmp(c); emit(c,"  %s = sitofp i32 %s to float\n",x,r); r=x; }
        const char *v=newtmp(c); *k=VK_I1;
        emit(c,"  %s = %s %s %s %s, %s\n",v,isf?"fcmp":"icmp",cmp_name(e->cmp,isf),isf?"float":"i32",l,r); return v; }
    case E_LOG:{ ValKind lk,rk; const char *l=gen_rval(c,e->lhs,&lk); const char *r=gen_rval(c,e->rhs,&rk);
        const char *v=newtmp(c); *k=VK_I1; emit(c,"  %s = %s i1 %s, %s\n",v,e->log==L_AND?"and":"or",l,r); return v; }
    case E_ASSIGN:{
        ValKind rk; const char *rhs=gen_rval(c,e->rhs,&rk);
        if(e->aop!=A_ASSIGN){
            ValKind lk; char *addr=gen_lval(c,e->operand,&lk,0); int pi=root_param(c,e->operand);
            const char *cur=newtmp(c);
            if(pi>=0) emit(c,"  %s = load float, float addrspace(%d)* %s, align 4\n",cur,c->fn->params[pi].ty.as,addr);
            else { int idx; resolve(c,e->operand->name,&idx);
                emit(c,"  %s = load %s, %s* %s, align 4\n",cur,scalar_ll(c->locs[idx].kind),scalar_ll(c->locs[idx].kind),addr); }
            int isf=(rk==VK_F32);
            const char *nv=newtmp(c);
            emit(c,"  %s = %s %s %s, %s\n",nv,as_op(e->aop,isf),isf?"float":"i32",cur,rhs); rhs=nv; rk=isf?VK_F32:VK_I32;
        }
        ValKind lk; char *addr=gen_lval(c,e->operand,&lk,1); int pi=root_param(c,e->operand);
        if(pi>=0){ c->written[pi]=1; const char *rv=coerce(c,rhs,rk,VK_F32);
            emit(c,"  store float %s, float addrspace(%d)* %s, align 4\n",rv,c->fn->params[pi].ty.as,addr); }
        else { int idx; resolve(c,e->operand->name,&idx); TypeKind kk=c->locs[idx].kind;
            const char *rv=coerce(c,rhs,rk,scalar_vk(kk));
            emit(c,"  store %s %s, %s* %s, align 4\n",scalar_ll(kk),rv,scalar_ll(kk),addr); }
        *k=rk; return rhs;
    }
    }
    die(0,"unreachable");
}
static char *gen_lval(CG *c, Expr *e, ValKind *k, int mark){
    if(e->kind==E_IDENT){
        int idx; RKind r=resolve(c,e->name,&idx);
        if(r==R_LOCAL){ *k=scalar_vk(c->locs[idx].kind); return c->locs[idx].slot; }
        die(0,"%s is not a mutable local",e->name);
    }
    if(e->kind==E_DEREF){ int pi=root_param(c,e); if(pi<0) die(0,"deref of non-parameter");
        if(mark)c->written[pi]=1; *k=VK_F32; return element_ptr(c,pi); }
    if(e->kind==E_FIELD){ int pi=root_param(c,e); if(pi<0) die(0,"-> on non-parameter");
        if(mark)c->written[pi]=1; Param *pr=&c->fn->params[pi]; StructDef *s=find_struct(c->prog,pr->ty.struct_name);
        int fi=0; for(size_t i=0;i<s->nfields;i++) if(!strcmp(s->fields[i].name,e->field))fi=(int)i;
        char *elt=element_ptr(c,pi); char *fp=newtmp(c); *k=VK_F32;
        emit(c,"  %s = getelementptr inbounds %%struct.%s, %%struct.%s addrspace(%d)* %s, i64 0, i32 %d\n",fp,pr->ty.struct_name,pr->ty.struct_name,pr->ty.as,elt,fi);
        return fp;
    }
    die(0,"not an lvalue");
}
/* produce an i1 condition from any expression */
static const char *gen_cond(CG *c, Expr *e){
    ValKind k; const char *v=gen_rval(c,e,&k);
    if(k==VK_I1) return v;
    const char *r=newtmp(c);
    emit(c,"  %s = %s %s %s %s, %s\n",r,k==VK_F32?"fcmp":"icmp",k==VK_F32?"one":"ne",k==VK_F32?"float":"i32",v,k==VK_F32?"0.000000e+00":"0");
    return r;
}
/* divergence heuristic: does expr touch device/constant element data (=> varying)? */
static int is_varying(CG *c, Expr *e){
    if(!e) return 0;
    if(e->kind==E_DEREF||e->kind==E_FIELD){ if(root_param(c,e)>=0) return 1; }
    return is_varying(c,e->operand)||is_varying(c,e->lhs)||is_varying(c,e->rhs);
}

static void gen_block(CG *c, Block *b);
static void gen_stmt(CG *c, Stmt *s){
    switch(s->kind){
    case S_EXPR:{ ValKind k; if(s->expr) gen_rval(c,s->expr,&k); break; }
    case S_DECL:{ TypeKind kk=s->ty.kind; const char *ll=scalar_ll(kk);
        char *slot=newtmp(c); sb_printf(c->pre,"  %s = alloca %s, align 4\n",slot,ll);
        c->locs=realloc(c->locs,(c->nlocs+1)*sizeof(Loc));
        c->locs[c->nlocs++]=(Loc){s->name,slot,kk};
        if(s->init){ ValKind k; const char *v=gen_rval(c,s->init,&k);
            const char *vv=coerce(c,v,k,scalar_vk(kk));
            emit(c,"  store %s %s, %s* %s, align 4\n",ll,vv,ll,slot); }
        break; }
    case S_RETURN:{ emit(c,"  ret void\n"); c->term=1; break; }
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
        lbl(c,body); gen_block(c,&s->then_b); if(!c->term) emit(c,"  br label %%bb%d\n",cond); lbl(c,en); break; }
    case S_FOR:{
        if(s->for_init) gen_stmt(c,s->for_init);
        int cond=newlbl(c), body=newlbl(c), inc=newlbl(c), en=newlbl(c);
        emit(c,"  br label %%bb%d\n",cond); lbl(c,cond);
        if(s->for_cond){ if(is_varying(c,s->for_cond)) fprintf(stderr,"binc: note: 'for' bound is data-dependent (varying) — per-thread loop; consider a uniform bound\n");
            const char *cv=gen_cond(c,s->for_cond); emit(c,"  br i1 %s, label %%bb%d, label %%bb%d\n",cv,body,en); }
        else emit(c,"  br label %%bb%d\n",body);
        lbl(c,body); gen_block(c,&s->then_b); if(!c->term) emit(c,"  br label %%bb%d\n",inc);
        lbl(c,inc); if(s->for_incr){ ValKind k; gen_rval(c,s->for_incr,&k); } if(!c->term) emit(c,"  br label %%bb%d\n",cond);
        lbl(c,en); break; }
    }
}
static void gen_block(CG *c, Block *b){ for(size_t i=0;i<b->n;i++) gen_stmt(c,&b->stmts[i]); }

static void fn_ptr_str(Function *fn,char *buf,size_t n){
    size_t o=0; o+=snprintf(buf+o,n-o,"void (");
    for(size_t i=0;i<fn->nparams;i++){ if(i)o+=snprintf(buf+o,n-o,", "); Param *p=&fn->params[i];
        if(p->ty.is_ptr){ char elt[64]; if(p->ty.kind==T_STRUCT)snprintf(elt,sizeof elt,"%%struct.%s",p->ty.struct_name);
            else snprintf(elt,sizeof elt,"%s",scalar_ll(p->ty.kind)); o+=snprintf(buf+o,n-o,"%s addrspace(%d)*",elt,p->ty.as); }
        else o+=snprintf(buf+o,n-o,"%s addrspace(2)*",scalar_ll(p->ty.kind)); }
    if(fn->nparams)o+=snprintf(buf+o,n-o,", "); o+=snprintf(buf+o,n-o,"i32)* @%s",fn->name);
}

void emit_air(FILE *out, const Program *prog){
    (void)0;
    fprintf(out,"; generated by binc — works as C, acts as Metal\n");
    fprintf(out,"target datalayout = \"e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32\"\n");
    fprintf(out,"target triple = \"air64_v29-apple-macosx27.0.0\"\n\n");
    for(size_t i=0;i<prog->nstructs;i++){ StructDef *s=&prog->structs[i];
        fprintf(out,"%%struct.%s = type { ",s->tag);
        for(size_t j=0;j<s->nfields;j++){ if(j)fprintf(out,", "); fprintf(out,"%s",scalar_ll(s->fields[j].ty.kind)); }
        fprintf(out," }\n"); } fprintf(out,"\n");

    typedef struct { int *read,*written; int np; } KF; KF *kf=calloc(prog->nfuncs,sizeof(KF));
    for(size_t fi=0;fi<prog->nfuncs;fi++){
        Function *fn=&prog->funcs[fi]; CG c={0};
        SB pr={0},bd={0}; c.pre=&pr; c.body=&bd; c.prog=prog; c.fn=fn; c.tmp=0;
        c.read=calloc(fn->nparams,sizeof(int)); c.written=calloc(fn->nparams,sizeof(int));
        c.scalar_load=calloc(fn->nparams,sizeof(char*));
        /* signature */
        char sig[2048]; size_t so=0; so+=snprintf(sig+so,sizeof sig-so,"define void @%s(",fn->name);
        for(size_t i=0;i<fn->nparams;i++){ Param *p=&fn->params[i]; if(i)so+=snprintf(sig+so,sizeof sig-so,", ");
            if(p->ty.is_ptr){ char elt[64]; if(p->ty.kind==T_STRUCT)snprintf(elt,sizeof elt,"%%struct.%s",p->ty.struct_name);
                else snprintf(elt,sizeof elt,"%s",scalar_ll(p->ty.kind)); so+=snprintf(sig+so,sizeof sig-so,"%s addrspace(%d)* nocapture noundef %%_%s",elt,p->ty.as,p->name); }
            else so+=snprintf(sig+so,sizeof sig-so,"%s addrspace(2)* nocapture noundef readonly align 4 dereferenceable(4) %%_%s",scalar_ll(p->ty.kind),p->name); }
        if(fn->nparams)so+=snprintf(sig+so,sizeof sig-so,", "); so+=snprintf(sig+so,sizeof sig-so,"i32 noundef %%_id) local_unnamed_addr #0 {\n");
        fprintf(out,"%s",sig);
        c.idx=malloc(16); snprintf(c.idx,16,"%%t%d",c.tmp++);
        sb_printf(c.pre,"  %s = zext i32 %%_id to i64\n",c.idx);
        gen_block(&c,&fn->body);
        if(!c.term) emit(&c,"  ret void\n");
        fwrite(pr.p,1,pr.n,out); fwrite(bd.p,1,bd.n,out);
        fprintf(out,"}\n\n");
        kf[fi].read=c.read; kf[fi].written=c.written; kf[fi].np=(int)fn->nparams;
    }
    fprintf(out,"attributes #0 = { argmemonly mustprogress nofree norecurse nosync nounwind willreturn \"no-trapping-math\"=\"true\" }\n\n");
    /* fully-numbered metadata */
    int next=13; int *knode=calloc(prog->nfuncs,sizeof(int)),*empty=calloc(prog->nfuncs,sizeof(int)),
        *arglist=calloc(prog->nfuncs,sizeof(int)),**argnode=calloc(prog->nfuncs,sizeof(int*)),*idnode=calloc(prog->nfuncs,sizeof(int));
    for(size_t fi=0;fi<prog->nfuncs;fi++){ knode[fi]=next++; empty[fi]=next++; arglist[fi]=next++;
        argnode[fi]=malloc(kf[fi].np*sizeof(int)); for(int a=0;a<kf[fi].np;a++)argnode[fi][a]=next++; idnode[fi]=next++; }
    fprintf(out,"!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6}\n");
    fprintf(out,"!air.kernel = !{"); for(size_t fi=0;fi<prog->nfuncs;fi++){ if(fi)fprintf(out,", "); fprintf(out,"!%d",knode[fi]); } fprintf(out,"}\n");
    fprintf(out,"!air.compile_options = !{!7, !8}\n!llvm.ident = !{!9}\n!air.version = !{!10}\n!air.language_version = !{!11}\n!air.source_file_name = !{!12}\n\n");
    fprintf(out,"!0 = !{i32 2, !\"SDK Version\", [2 x i32] [i32 27, i32 0]}\n!1 = !{i32 1, !\"wchar_size\", i32 4}\n!2 = !{i32 7, !\"air.max_device_buffers\", i32 31}\n!3 = !{i32 7, !\"air.max_constant_buffers\", i32 31}\n!4 = !{i32 7, !\"air.max_threadgroup_buffers\", i32 31}\n!5 = !{i32 7, !\"air.max_textures\", i32 128}\n!6 = !{i32 7, !\"air.max_samplers\", i32 16}\n!7 = !{!\"air.compile.denorms_disable\"}\n!8 = !{!\"air.compile.fast_math_enable\"}\n!9 = !{!\"BinC compiler v0.0.1 (bootstrap, in C)\"}\n!10 = !{i32 2, i32 9, i32 0}\n!11 = !{!\"Metal\", i32 4, i32 1, i32 0}\n!12 = !{!\"binc\"}\n");
    for(size_t fi=0;fi<prog->nfuncs;fi++){ Function *fn=&prog->funcs[fi]; char fptr[2048]; fn_ptr_str(fn,fptr,sizeof fptr);
        fprintf(out,"!%d = !{}\n",empty[fi]);
        fprintf(out,"!%d = !{",arglist[fi]); for(int a=0;a<kf[fi].np;a++){ if(a)fprintf(out,", "); fprintf(out,"!%d",argnode[fi][a]); }
        if(kf[fi].np)fprintf(out,", "); fprintf(out,"!%d}\n",idnode[fi]);
        fprintf(out,"!%d = !{%s, !%d, !%d}\n",knode[fi],fptr,empty[fi],arglist[fi]);
        for(int a=0;a<kf[fi].np;a++){ Param *p=&fn->params[a];
            if(p->ty.is_ptr){ int r=kf[fi].read[a],w=kf[fi].written[a]; const char *acc=(r&&w)?"air.read_write":w?"air.write":"air.read";
                int sz=4,al=4; const char *tn=scalar_ll(p->ty.kind); char tnb[64];
                if(p->ty.kind==T_STRUCT){ StructDef *s=find_struct(prog,p->ty.struct_name); sz=(int)s->nfields*4; al=4; snprintf(tnb,sizeof tnb,"%s",p->ty.struct_name); tn=tnb; }
                fprintf(out,"!%d = !{i32 %d, !\"air.buffer\", !\"air.location_index\", i32 %d, i32 1, !\"%s\", !\"air.address_space\", i32 %d, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",argnode[fi][a],a,a,acc,p->ty.as,sz,al,tn,p->name); }
            else fprintf(out,"!%d = !{i32 %d, !\"air.buffer\", !\"air.buffer_size\", i32 4, !\"air.location_index\", i32 %d, i32 1, !\"air.read\", !\"air.address_space\", i32 2, !\"air.arg_type_size\", i32 4, !\"air.arg_type_align_size\", i32 4, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",argnode[fi][a],a,a,scalar_ll(p->ty.kind),p->name); }
        fprintf(out,"!%d = !{i32 %d, !\"air.thread_position_in_grid\", !\"air.arg_type_name\", !\"uint\", !\"air.arg_name\", !\"id\"}\n",idnode[fi],(int)fn->nparams);
    }
}
