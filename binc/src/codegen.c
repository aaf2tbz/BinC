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
/* matrix layout: N column vectors; AIR vector alignment gives the MSL-compatible
 * padded stride (mat3 columns align to 16, like float3 columns in MSL matrices) */
static int mat_size(int matn){ return matn==2?16:matn==3?48:64; }
static int mat_align(int matn){ return matn==2?8:16; }
static int tsz(TypeKind k,int vecn,int matn){ return matn?mat_size(matn):type_size(k,vecn); }
static int tal(TypeKind k,int vecn,int matn){ return matn?mat_align(matn):type_align(k,vecn); }
static void mll_of(char *buf,size_t n,int matn){ snprintf(buf,n,"[%d x <%d x float>]",matn,matn); }
static void ll_of(char *buf,size_t n,TypeKind k,int vecn);
/* matrix-aware value type: matrix -> [N x <N x float>], else scalar/vector */
static void pll_of(char *buf,size_t n,TypeKind k,int vecn,int matn){
    if(matn) mll_of(buf,n,matn); else ll_of(buf,n,k,vecn); }
static void ll_of(char *buf,size_t n,TypeKind k,int vecn){
    if(vecn>1) snprintf(buf,n,"<%d x %s>",vecn,scalar_ll(k)); else snprintf(buf,n,"%s",scalar_ll(k)); }
static void type_ll(char *buf,size_t n,TypeKind k,char *sname,int vecn){
    if(k==T_STRUCT) snprintf(buf,n,"%%struct.%s",sname);
    else if(k==T_ATOMIC) snprintf(buf,n,"%%\"struct.metal::_atomic\"");
    else ll_of(buf,n,k,vecn);
}
static void tn_of(char *buf,size_t n,TypeKind k,int vecn){
    if(vecn>1) snprintf(buf,n,"%s%d",type_name(k),vecn); else snprintf(buf,n,"%s",type_name(k)); }
static void ptn_of(char *buf,size_t n,TypeKind k,int vecn,int matn){
    if(matn) snprintf(buf,n,"mat%d",matn); else tn_of(buf,n,k,vecn); }
/* real struct size (tail-padded) and alignment per the AIR datalayout */
static int struct_layout(StructDef *s, int *al){
    int off=0,m=1;
    for(size_t i=0;i<s->nfields;i++){ int fa=tal(s->fields[i].ty.kind,s->fields[i].ty.vecn,s->fields[i].ty.matn);
        off=(off+fa-1)&~(fa-1); off+=tsz(s->fields[i].ty.kind,s->fields[i].ty.vecn,s->fields[i].ty.matn); if(fa>m)m=fa; }
    *al=m; return (off+m-1)&~(m-1);
}

typedef struct { char *name; char *slot; TypeKind kind; char *sname; int vecn; int matn; int is_const; } Loc;
typedef struct {
    SB *pre,*body; const Program *prog; Function *fn;
    int tmp; char *idx; int explicit_domain; int coord_param; int grid_extent_param;
    int uses_local_coord, uses_group_coord; int *read,*written; char **scalar_load;
    Loc *locs; size_t nlocs;
    int term; /* current block terminated */
    int blk_empty; /* current block has no instructions yet (AIR rejects empty blocks) */
    int lblc; /* label counter (separate from tmp) */
    int brk_l[32], cont_l[32], nloops; /* break/continue label stack */
    int uses_sync; /* body contains a barrier -> function must be convergent, not nosync */
    int divergent; /* current control-flow region is varying/divergent */
    int rvw; /* vector width of the value gen_rval just returned (0 = scalar) */
    int rmat; /* matrix width of the value gen_rval just returned (0 = not a matrix) */
} CG;
static char *newtmp(CG *c){ char *s=malloc(16); snprintf(s,16,"%%t%d",c->tmp++); return s; }
static int newlbl(CG *c){ return c->lblc++; }
static void emit(CG *c,const char *fmt,...){ va_list a; va_start(a,fmt); char b[1024]; vsnprintf(b,sizeof b,fmt,a); va_end(a); sb_put(c->body,b); c->blk_empty=0; }
static void lbl(CG *c,int n){
    if(c->blk_empty) emit(c,"  br label %%bb%d\n",n); /* AIR rejects empty blocks: jump from the empty predecessor */
    char b[32]; snprintf(b,sizeof b,"bb%d:\n",n); sb_put(c->body,b); c->term=0; c->blk_empty=1;
}
/* materialize a float constant as a register. AIR rejects floating-point
 * literals as instruction operands, so every FP constant is carried as
 * bitcast(i32) — same bits, always a valid SSA operand. */
static const char *fconst(CG *c, double v){
    float fv=(float)v; unsigned bits; memcpy(&bits,&fv,4);
    const char *r=newtmp(c);
    emit(c,"  %s = bitcast i32 %u to float\n",r,bits);
    return r;
}
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

/* resolve a name to: local | pointer param idx | scalar param idx | module constant | texture/sampler */
typedef enum { R_NONE,R_LOCAL,R_PTR,R_SCALAR,R_COORD,R_EXTENT,R_CONST,R_TEXTURE,R_SAMPLER } RKind;
static RKind resolve(CG *c, const char *name, int *out){
    for(size_t i=0;i<c->nlocs;i++) if(!strcmp(c->locs[i].name,name)){ *out=(int)i; return R_LOCAL; }
    for(size_t i=0;i<c->fn->nparams;i++) if(!strcmp(c->fn->params[i].name,name)){
        *out=(int)i;
        if(c->fn->params[i].ty.kind==T_COORD) return R_COORD;
        if(c->fn->params[i].ty.kind==T_GRID_EXTENT) return R_EXTENT;
        if(c->fn->params[i].ty.kind==T_TEXTURE) return R_TEXTURE;
        if(c->fn->params[i].ty.kind==T_SAMPLER) return R_SAMPLER;
        return c->fn->params[i].ty.is_ptr||c->fn->params[i].ty.array_n?R_PTR:R_SCALAR; }
    for(size_t i=0;i<c->prog->nconsts;i++) if(!strcmp(c->prog->consts[i].name,name)){ *out=(int)i; return R_CONST; }
    return R_NONE;
}
static int root_param(CG *c, Expr *e){
    while(e&&(e->kind==E_DEREF||e->kind==E_FIELD||e->kind==E_INDEX)) e=e->operand;
    if(e&&e->kind==E_IDENT){ int idx; if(resolve(c,e->name,&idx)==R_PTR) return idx; } return -1; }
static const char *coord_ll(Type *t, char *buf, size_t n){
    if(t->coordn==1) snprintf(buf,n,"i32");
    else snprintf(buf,n,"<%d x i16>",t->coordn);
    return buf;
}
static int coord_width(Type *t){ return t->coordn==1?0:t->coordn; }
static const char *coord_value(CG *c, Type *t, const char *raw){
    if(t->coordn==1) return strdup(raw);
    const char *v=newtmp(c); emit(c,"  %s = zext <%d x i16> %s to <%d x i32>\n",v,t->coordn,raw,t->coordn); return v;
}

/* lvalue description: element type + where the address lives */
typedef struct { TypeKind tk; char *sname; int as; int pi; int is_local; int vecn; int matn; } LInfo;
static void shared_name(CG *c, int pi, char *buf, size_t n){
    snprintf(buf,n,"@_binc_smem_%s_%s",c->fn->name,c->fn->params[pi].name);
}
static void pty_str(char *buf,size_t n,TypeKind tk,char *sname,int as,int is_local,int vecn,int matn){
    char elt[64];
    if(matn) mll_of(elt,sizeof elt,matn); else type_ll(elt,sizeof elt,tk,sname,vecn);
    if(is_local) snprintf(buf,n,"%s*",elt); else snprintf(buf,n,"%s addrspace(%d)*",elt,as); }

/* address of element p[ix] (i64 index value) for pointer param pi */
static char *element_ptr_idx(CG *c,int pi,const char *ix){
    Param *pr=&c->fn->params[pi]; char elt[64];
    if(pr->ty.matn) mll_of(elt,sizeof elt,pr->ty.matn); else type_ll(elt,sizeof elt,pr->ty.kind,pr->ty.struct_name,pr->ty.vecn);
    char *s=newtmp(c);
    emit(c,"  %s = getelementptr inbounds %s, %s addrspace(%d)* %%_%s, i64 %s\n",s,elt,elt,pr->ty.as,pr->name,ix);
    return s;
}
static char *gen_lval(CG *c, Expr *e, LInfo *li, int mark);
static const char *gen_cond(CG *c, Expr *e);
static const char *gen_rval(CG *c, Expr *e, ValKind *k);

/* matrix width of a name/field expression, 0 if not a matrix */
static int matrix_n(CG *c, Expr *e){
    if(e->kind==E_IDENT){
        int i; RKind r=resolve(c,e->name,&i);
        if(r==R_LOCAL) return c->locs[i].matn;
        if(r==R_PTR) return c->fn->params[i].ty.matn;
        return 0;
    }
    if(e->kind==E_FIELD && e->operand->kind==E_IDENT){
        int i; if(resolve(c,e->operand->name,&i)!=R_LOCAL) return 0;
        if(c->locs[i].kind!=T_STRUCT) return 0;
        StructDef *s=find_struct(c->prog,c->locs[i].sname);
        if(!s) return 0;
        for(size_t f=0;f<s->nfields;f++) if(!strcmp(s->fields[f].name,e->field)) return s->fields[f].ty.matn;
        return 0;
    }
    return 0;
}
/* evaluate an expression to a matrix register ([N x <N x float>]) */
static const char *mat_eval(CG *c, Expr *e, int *mn){
    ValKind k; const char *r=gen_rval(c,e,&k);
    *mn=c->rmat;
    if(!*mn) die(0,"expected a matrix");
    return r;
}
/* extract column `col` of a matrix register */
static const char *mat_col(CG *c, const char *m, int mn, int col){
    char mty[32]; mll_of(mty,sizeof mty,mn);
    const char *r=newtmp(c);
    emit(c,"  %s = extractvalue %s %s, %d\n",r,mty,m,col);
    return r;
}
/* insert column `col` into a matrix aggregate */
static const char *mat_setcol(CG *c, const char *m, int mn, int col, const char *v){
    char mty[32]; mll_of(mty,sizeof mty,mn);
    const char *r=newtmp(c);
    emit(c,"  %s = insertvalue %s %s, <%d x float> %s, %d\n",r,mty,m,mn,v,col);
    return r;
}
/* element of a matrix register at column col, row r */
static const char *mat_elem(CG *c, const char *m, int mn, int col, int r){
    const char *colv=mat_col(c,m,mn,col);
    const char *x=newtmp(c);
    emit(c,"  %s = extractelement <%d x float> %s, i32 %d\n",x,mn,colv,r);
    return x;
}

static const char *gen_rval(CG *c, Expr *e, ValKind *k);

/* pointer expression -> owning param + i64 index value; bare `p` means the implicit thread index */
static int eval_ptr(CG *c, Expr *e, const char **ix){
    if(e->kind==E_IDENT){ int i; if(resolve(c,e->name,&i)==R_PTR){
        if(!c->fn->is_kernel||!c->idx) die(0,"implicit *%s needs a scalar kernel thread id; subscript explicitly: %s[i]",e->name,e->name);
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
    li->tk=pr->ty.kind; li->sname=pr->ty.struct_name; li->as=pr->ty.as; li->pi=pi; li->is_local=0; li->vecn=pr->ty.vecn; li->matn=pr->ty.matn; }

/* load an lvalue, promoting half to float; *k gets the expression-level kind, c->rvw the vector width */
static const char *emit_load_t(CG *c, LInfo *li, const char *addr, ValKind *k){
    if(li->tk==T_STRUCT) die(0,"cannot use a whole struct as a value");
    char pty[96]; pty_str(pty,sizeof pty,li->tk,li->sname,li->as,li->is_local,li->vecn,li->matn);
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
/* component name -> index, or -1; multi-char swizzles return -1 here */
static int comp_idx(const char *f){
    const char *names="xyzw"; const char *rgba="rgba";
    const char *p=strchr(names,f[0]); if(!p) p=strchr(rgba,f[0]);
    if(!p||f[1]) return -1; return (int)(p-(p==strchr(names,f[0])?names:rgba))%4;
}
/* swizzle "xy"/"wzyx"/"rga": up to 4 component indices; -1 if invalid */
static int swizzle_idx(const char *f, int *out){
    size_t l=strlen(f);
    if(l<1||l>4) return -1;
    const char *names="xyzwrgba";
    for(size_t i=0;i<l;i++){
        const char *p=strchr(names,f[i]);
        if(!p) return -1;
        int pos=(int)(p-names);
        out[i]=pos>=4?pos-4:pos;
    }
    return (int)l;
}
/* build <nc x elt> from component indices of a vector register */
static const char *swizzle_read(CG *c, const char *vec, const char *vty, const char *ety, const int *idxs, int nc){
    const char *acc="undef";
    for(int i=0;i<nc;i++){
        const char *x=newtmp(c);
        emit(c,"  %s = extractelement %s %s, i32 %d\n",x,vty,vec,idxs[i]);
        char ty[32]; snprintf(ty,sizeof ty,"<%d x %s>",nc,ety);
        const char *r=newtmp(c);
        emit(c,"  %s = insertelement %s %s, %s %s, i32 %d\n",r,ty,acc,ety,x,i);
        acc=r;
    }
    return acc;
}

/* builtins: AIR spellings probed from the metal frontend (metal -emit-llvm -S on MSL using
 * metal::sqrt etc.; the module sets air.compile.fast_math_enable, hence the fast_ variants).
 * sync() is the threadgroup barrier: air.wg.barrier(i32 2, i32 5, i32 1). */
typedef struct { const char *name,*ll; int nargs; TypeKind ret, a0, a1; int vec_ok; } Builtin;
static Builtin builtins[]={
    {"sqrt","air.fast_sqrt.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"fabs","air.fast_fabs.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"floor","air.fast_floor.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"ceil","air.fast_ceil.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"sin","air.fast_sin.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"cos","air.fast_cos.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"exp","air.fast_exp.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"log","air.fast_log.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"fmin","air.fast_fmin.f32",2,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"fmax","air.fast_fmax.f32",2,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"pow","air.fast_pow.f32",2,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"atan2","air.fast_atan2.f32",2,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"rsqrt","air.fast_rsqrt.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"sign","air.sign.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"imin","air.min.s.i32",2,T_INT32,T_INT32,T_INT32,1},
    {"imax","air.max.s.i32",2,T_INT32,T_INT32,T_INT32,1},
    {"sync","air.wg.barrier",0,T_VOID,T_VOID,T_VOID,0},
};
static int builtin_used[sizeof builtins/sizeof *builtins];
static void mark_builtin(const char *name){ for(size_t i=0;i<sizeof builtins/sizeof *builtins;i++) if(!strcmp(builtins[i].name,name)){ builtin_used[i]=1; return; } }
static int atomic_add_used[3];
static int tex_read_used[4], tex_write_used[4], tex_sample_used[4], get_samp_used;
static void tex_kinds(TypeKind et, const char **elt, const char **vec, const char **suf, const char **an){
    if(et==T_HALF){ *elt="half"; *vec="<4 x half>"; *suf="v4f16"; *an="half4"; }
    else if(et==T_INT32){ *elt="i32"; *vec="<4 x i32>"; *suf="v4i32"; *an="int4"; }
    else if(et==T_UINT32){ *elt="i32"; *vec="<4 x i32>"; *suf="v4u32"; *an="uint4"; }
    else { *elt="float"; *vec="<4 x float>"; *suf="v4f32"; *an="float4"; }
}

/* ---- composite math builtins ----
 * dot/cross/length/... are composed from elementwise intrinsics and vector
 * arithmetic; they accept float scalars and vectors of matching width. */
static const char *vbin(CG *c, const char *op, const char *a, const char *b, int n){
    if(n>1){ const char *r=newtmp(c); emit(c,"  %s = %s <%d x float> %s, %s\n",r,op,n,a,b); return r; }
    const char *r=newtmp(c); emit(c,"  %s = %s float %s, %s\n",r,op,a,b); return r;
}
/* sum of every element -> scalar float */
static const char *vreduce(CG *c, const char *v, int n){
    if(n==1) return v;
    const char *acc=NULL;
    for(int i=0;i<n;i++){
        const char *x=newtmp(c);
        emit(c,"  %s = extractelement <%d x float> %s, i32 %d\n",x,n,v,i);
        if(!acc) acc=x;
        else { const char *r=newtmp(c); emit(c,"  %s = fadd fast float %s, %s\n",r,acc,x); acc=r; }
    }
    return acc;
}
/* elementwise scalar intrinsic call on a vector register (or plain scalar) */
static const char *elem_call(CG *c, const char *intr, const char *v, int n, TypeKind at, TypeKind rt){
    if(n==1){
        const char *r=newtmp(c);
        emit(c,"  %s = call %s @%s(%s %s)\n",r,scalar_ll(rt),intr,scalar_ll(at),v);
        return r;
    }
    const char *acc="undef";
    char rty[32], vty[32]; snprintf(rty,sizeof rty,"<%d x %s>",n,scalar_ll(rt)); snprintf(vty,sizeof vty,"<%d x %s>",n,scalar_ll(at));
    for(int i=0;i<n;i++){
        const char *x=newtmp(c);
        emit(c,"  %s = extractelement %s %s, i32 %d\n",x,vty,v,i);
        const char *r=newtmp(c);
        emit(c,"  %s = call %s @%s(%s %s)\n",r,scalar_ll(rt),intr,scalar_ll(at),x);
        const char *ins=newtmp(c);
        emit(c,"  %s = insertelement %s %s, %s %s, i32 %d\n",ins,rty,acc,scalar_ll(rt),r,i);
        acc=ins;
    }
    return acc;
}
/* clamp v to [lo, hi] elementwise; lo/hi may be vector registers or splatted scalars */
static const char *clamp_elem(CG *c, const char *v, const char *lo, const char *hi, int n){
    mark_builtin("fmin"); mark_builtin("fmax");
    const char *acc="undef";
    char rty[32]; snprintf(rty,sizeof rty,"<%d x float>",n);
    for(int i=0;i<n;i++){
        const char *x=v, *l=lo, *h=hi;
        if(n>1){
            x=newtmp(c); l=newtmp(c); h=newtmp(c);
            emit(c,"  %s = extractelement <%d x float> %s, i32 %d\n",x,n,v,i);
            emit(c,"  %s = extractelement <%d x float> %s, i32 %d\n",l,n,lo,i);
            emit(c,"  %s = extractelement <%d x float> %s, i32 %d\n",h,n,hi,i);
        }
        const char *mn=newtmp(c);
        emit(c,"  %s = call float @air.fast_fmin.f32(float %s, float %s)\n",mn,x,h);
        const char *mx=newtmp(c);
        emit(c,"  %s = call float @air.fast_fmax.f32(float %s, float %s)\n",mx,l,mn);
        if(n>1){ const char *ins=newtmp(c); emit(c,"  %s = insertelement %s %s, float %s, i32 %d\n",ins,rty,acc,mx,i); acc=ins; }
        else acc=mx;
    }
    return acc;
}
/* scalars are broadcast to width n for vector arithmetic */
static const char *spread(CG *c, const char *v, int w, int n){
    return (w||n==1)?v:splat(c,v,"float",n);
}
/* returns the result register, or NULL if `e->name` is not a composite builtin */
static const char *gen_composite_math(CG *c, Expr *e, ValKind *k){
    const char *nm=e->name;
    static const char *names[]={"dot","cross","length","distance","normalize","reflect","clamp","mix","step","smoothstep","fract","mod","radians","degrees"};
    int hit=0; for(size_t i=0;i<sizeof names/sizeof *names;i++) if(!strcmp(names[i],nm)){ hit=1; break; }
    if(!hit) return NULL;
    if(e->nargs<1||e->nargs>3) die(0,"%s: wrong number of arguments",nm);
    ValKind ak=VK_F32,bk=VK_F32,ck=VK_F32;
    const char *av=gen_rval(c,e->args[0],&ak); int aw=c->rvw;
    const char *bv=NULL; int bw=0; if(e->nargs>=2){ bv=gen_rval(c,e->args[1],&bk); bw=c->rvw; }
    const char *cv=NULL; int cw=0; if(e->nargs>=3){ cv=gen_rval(c,e->args[2],&ck); cw=c->rvw; }
    int n=aw?aw:(bw?bw:cw); if(n==0) n=1;
    if((aw&&aw!=n)||(bw&&bw!=n)||(cw&&cw!=n)) die(0,"vector width mismatch in %s",nm);
    if(ak!=VK_F32||bk!=VK_F32||ck!=VK_F32) die(0,"%s requires float arguments",nm);
    if(!strcmp(nm,"dot")){
        if(e->nargs!=2) die(0,"dot expects 2 arguments");
        *k=VK_F32; c->rvw=0;
        return vreduce(c,vbin(c,"fmul fast",spread(c,av,aw,n),spread(c,bv,bw,n),n),n);
    }
    if(!strcmp(nm,"cross")){
        if(e->nargs!=2) die(0,"cross expects 2 arguments");
        if(n!=3) die(0,"cross requires float3 arguments");
        int s1[3]={1,2,0}, s2[3]={2,0,1};
        const char *ayz=swizzle_read(c,av,"<3 x float>","float",s1,3);
        const char *azy=swizzle_read(c,av,"<3 x float>","float",s2,3);
        const char *byz=swizzle_read(c,bv,"<3 x float>","float",s1,3);
        const char *bzy=swizzle_read(c,bv,"<3 x float>","float",s2,3);
        *k=VK_F32; c->rvw=3;
        return vbin(c,"fsub fast",vbin(c,"fmul fast",ayz,bzy,3),vbin(c,"fmul fast",azy,byz,3),3);
    }
    if(!strcmp(nm,"length")||!strcmp(nm,"distance")){
        if(e->nargs!=(!strcmp(nm,"length")?1:2)) die(0,"%s: wrong number of arguments",nm);
        mark_builtin("sqrt");
        const char *va=spread(c,av,aw,n);
        const char *vb = !strcmp(nm,"distance") ? spread(c,bv,bw,n) : NULL;
        const char *d;
        if(vb) d=vreduce(c,vbin(c,"fmul fast",vbin(c,"fsub fast",vb,va,n),vbin(c,"fsub fast",vb,va,n),n),n);
        else d=vreduce(c,vbin(c,"fmul fast",va,va,n),n);
        *k=VK_F32; c->rvw=0;
        const char *r=newtmp(c);
        emit(c,"  %s = call float @air.fast_sqrt.f32(float %s)\n",r,d);
        return r;
    }
    if(!strcmp(nm,"normalize")){
        if(e->nargs!=1) die(0,"normalize expects 1 argument");
        mark_builtin("sqrt");
        const char *va=spread(c,av,aw,n);
        const char *d=vreduce(c,vbin(c,"fmul fast",va,va,n),n);
        const char *r=newtmp(c);
        emit(c,"  %s = call float @air.fast_sqrt.f32(float %s)\n",r,d);
        const char *len=n>1?splat(c,r,"float",n):r;
        *k=VK_F32; c->rvw=n>1?n:0;
        return vbin(c,"fdiv fast",va,len,n);
    }
    if(!strcmp(nm,"reflect")){
        if(e->nargs!=2) die(0,"reflect expects 2 arguments");
        const char *va=spread(c,av,aw,n), *vb=spread(c,bv,bw,n);
        const char *d=vreduce(c,vbin(c,"fmul fast",vb,va,n),n);   /* dot(n,i) */
        const char *t2=newtmp(c); emit(c,"  %s = fmul fast float %s, %s\n",t2,fconst(c,2.0),d);
        const char *tv=n>1?splat(c,t2,"float",n):t2;
        *k=VK_F32; c->rvw=n>1?n:0;
        return vbin(c,"fsub fast",va,vbin(c,"fmul fast",vb,tv,n),n);
    }
    if(!strcmp(nm,"clamp")){
        if(e->nargs!=3) die(0,"clamp expects 3 arguments");
        *k=VK_F32; c->rvw=n>1?n:0;
        return clamp_elem(c,spread(c,av,aw,n),spread(c,bv,bw,n),spread(c,cv,cw,n),n);
    }
    if(!strcmp(nm,"mix")){
        if(e->nargs!=3) die(0,"mix expects 3 arguments");
        const char *va=spread(c,av,aw,n), *vb=spread(c,bv,bw,n), *vc=spread(c,cv,cw,n);
        *k=VK_F32; c->rvw=n>1?n:0;
        return vbin(c,"fadd fast",va,vbin(c,"fmul fast",vbin(c,"fsub fast",vb,va,n),vc,n),n);
    }
    if(!strcmp(nm,"step")){
        if(e->nargs!=2) die(0,"step expects 2 arguments");
        const char *va=spread(c,av,aw,n), *vb=spread(c,bv,bw,n);
        const char *zero=fconst(c,0.0), *one=fconst(c,1.0);
        if(n>1){
            const char *m=newtmp(c);
            emit(c,"  %s = fcmp olt <%d x float> %s, %s\n",m,n,vb,va);
            const char *z=splat(c,zero,"float",n), *o=splat(c,one,"float",n);
            char mty[32], rty[32]; snprintf(mty,sizeof mty,"<%d x i1>",n); snprintf(rty,sizeof rty,"<%d x float>",n);
            const char *r=newtmp(c);
            emit(c,"  %s = select %s %s, %s %s, %s %s\n",r,mty,m,rty,z,rty,o);
            *k=VK_F32; c->rvw=n; return r;
        }
        const char *m=newtmp(c);
        emit(c,"  %s = fcmp olt float %s, %s\n",m,vb,va);
        const char *r=newtmp(c);
        emit(c,"  %s = select i1 %s, float %s, float %s\n",r,m,zero,one);
        *k=VK_F32; c->rvw=0; return r;
    }
    if(!strcmp(nm,"smoothstep")){
        if(e->nargs!=3) die(0,"smoothstep expects 3 arguments");
        const char *va=spread(c,av,aw,n), *vb=spread(c,bv,bw,n), *vc=spread(c,cv,cw,n);
        const char *num=vbin(c,"fsub fast",vc,va,n);
        const char *den=vbin(c,"fsub fast",vb,va,n);
        const char *t0=vbin(c,"fdiv fast",num,den,n);
        const char *lo=fconst(c,0.0), *hi=fconst(c,1.0);
        const char *tc=clamp_elem(c,t0,lo,hi,n);
        const char *tt=vbin(c,"fmul fast",tc,tc,n);
        const char *t3=n>1?splat(c,fconst(c,3.0),"float",n):fconst(c,3.0);
        const char *t2=n>1?splat(c,fconst(c,2.0),"float",n):fconst(c,2.0);
        *k=VK_F32; c->rvw=n>1?n:0;
        return vbin(c,"fmul fast",tt,vbin(c,"fsub fast",t3,vbin(c,"fmul fast",t2,tc,n),n),n);
    }
    if(!strcmp(nm,"fract")){
        if(e->nargs!=1) die(0,"fract expects 1 argument");
        mark_builtin("floor");
        const char *va=spread(c,av,aw,n);
        const char *fl=elem_call(c,"air.fast_floor.f32",va,n,T_FLOAT,T_FLOAT);
        *k=VK_F32; c->rvw=n>1?n:0;
        return vbin(c,"fsub fast",va,fl,n);
    }
    if(!strcmp(nm,"mod")){
        if(e->nargs!=2) die(0,"mod expects 2 arguments");
        mark_builtin("floor");
        const char *va=spread(c,av,aw,n), *vb=spread(c,bv,bw,n);
        const char *q=vbin(c,"fdiv fast",va,vb,n);
        const char *fl=elem_call(c,"air.fast_floor.f32",q,n,T_FLOAT,T_FLOAT);
        *k=VK_F32; c->rvw=n>1?n:0;
        return vbin(c,"fsub fast",va,vbin(c,"fmul fast",vb,fl,n),n);
    }
    if(!strcmp(nm,"radians")||!strcmp(nm,"degrees")){
        if(e->nargs!=1) die(0,"%s expects 1 argument",nm);
        const char *va=spread(c,av,aw,n);
        const char *f=fconst(c,!strcmp(nm,"radians")?0.017453292519943295:57.29577951308232);
        const char *fc=n>1?splat(c,f,"float",n):f;
        *k=VK_F32; c->rvw=n>1?n:0;
        return vbin(c,"fmul fast",va,fc,n);
    }
    die(0,"unreachable in %s",nm);
}

static const char *cmp_name(CmpOp op,int isfloat,int isuns){
    if(isfloat) switch(op){ case C_EQ:return "oeq"; case C_NE:return "one"; case C_LT:return "olt";
        case C_LE:return "ole"; case C_GT:return "ogt"; case C_GE:return "oge"; }
    if(isuns) switch(op){ case C_EQ:return "eq"; case C_NE:return "ne"; case C_LT:return "ult";
        case C_LE:return "ule"; case C_GT:return "ugt"; case C_GE:return "uge"; }
    switch(op){ case C_EQ:return "eq"; case C_NE:return "ne"; case C_LT:return "slt";
        case C_LE:return "sle"; case C_GT:return "sgt"; case C_GE:return "sge"; } return "eq";
}
static int bitwise_bin(BinOp b){ return b==B_AND||b==B_OR||b==B_XOR||b==B_SHL||b==B_SHR; }
static const char *bin_op(BinOp b, int isf, int uns){
    if(isf) switch(b){ case B_ADD:return "fadd fast"; case B_SUB:return "fsub fast"; case B_MUL:return "fmul fast";
        case B_DIV:return "fdiv fast"; case B_MOD:return "frem fast"; default:return NULL; }
    switch(b){ case B_ADD:return "add"; case B_SUB:return "sub"; case B_MUL:return "mul";
        case B_DIV:return uns?"udiv":"sdiv"; case B_MOD:return uns?"urem":"srem";
        case B_AND:return "and"; case B_OR:return "or"; case B_XOR:return "xor";
        case B_SHL:return "shl"; case B_SHR:return uns?"lshr":"ashr"; default:return NULL; }
}
static int bitwise_assign(AssignOp a){ return a==A_ANDEQ||a==A_OREQ||a==A_XOREQ||a==A_SHLEQ||a==A_SHREQ; }
/* warn when an implicit numeric conversion can lose precision.
 * explicit casts (E_CAST) never route through here. small int constants
 * are exactly representable as float and stay silent. */
static void warn_implicit(CG *c, Expr *src, ValKind from, TypeKind tk, int vecn){
    (void)c; (void)vecn;
    ValKind to=scalar_vk(tk);
    if(from==to) return;
    if((from==VK_I32&&to==VK_U32)||(from==VK_U32&&to==VK_I32)) return;
    if(from==VK_I1) return;
    if(from==VK_F32&&(to==VK_I32||to==VK_U32)){
        fprintf(stderr,"binc: note (line %d): implicit float->int conversion truncates; cast explicitly to silence\n",src->line);
        return;
    }
    if((from==VK_I32||from==VK_U32)&&to==VK_F32){
        if(src->kind==E_ICONST && src->ival>-16777216 && src->ival<16777216) return;
        fprintf(stderr,"binc: note (line %d): implicit int->float conversion may lose precision; cast explicitly to silence\n",src->line);
    }
}
static const char *as_op(AssignOp a, int isfloat, int isuns){
    if(isfloat) switch(a){ case A_ADDEQ:return "fadd"; case A_SUBEQ:return "fsub"; case A_MULEQ:return "fmul";
        case A_DIVEQ:return "fdiv"; case A_MODEQ:return "frem"; default:return NULL; }
    switch(a){ case A_ADDEQ:return "add"; case A_SUBEQ:return "sub"; case A_MULEQ:return "mul";
        case A_DIVEQ:return isuns?"udiv":"sdiv"; case A_MODEQ:return isuns?"urem":"srem";
        case A_ANDEQ:return "and"; case A_OREQ:return "or"; case A_XOREQ:return "xor";
        case A_SHLEQ:return "shl"; case A_SHREQ:return isuns?"lshr":"ashr"; default:return NULL; }
}

static const char *gen_rval(CG *c, Expr *e, ValKind *k){
    g_last_line=e->line; g_last_col=e->col;
    c->rvw=0; c->rmat=0; /* cases set these to the width of their result, if any */
    switch(e->kind){
    case E_FCONST:{ *k=VK_F32; return fconst(c,e->fval); }
    case E_ICONST:{ *k=VK_I32; char *s=malloc(16); snprintf(s,16,"%ld",e->ival); return s; }
    case E_BOOL:{ *k=VK_I1; return e->bval?"true":"false"; }
    case E_IDENT:{
        int idx; RKind r=resolve(c,e->name,&idx);
        if(r==R_LOCAL){
            if(c->locs[idx].matn){
                /* whole matrix value: aggregate load */
                char mty[32]; mll_of(mty,sizeof mty,c->locs[idx].matn);
                const char *v=newtmp(c);
                emit(c,"  %s = load %s, %s* %s, align %d\n",v,mty,mty,c->locs[idx].slot,mat_align(c->locs[idx].matn));
                *k=VK_F32; c->rvw=0; c->rmat=c->locs[idx].matn; return v;
            }
            LInfo li={c->locs[idx].kind,c->locs[idx].sname,0,-1,1,c->locs[idx].vecn,c->locs[idx].matn};
            return emit_load_t(c,&li,c->locs[idx].slot,k); }
        if(r==R_COORD){
            Param *p=&c->fn->params[idx]; char nm[32]; snprintf(nm,sizeof nm,"%%_%s",p->name);
            *k=VK_U32; c->rvw=coord_width(&p->ty); return coord_value(c,&p->ty,nm);
        }
        if(r==R_EXTENT){ *k=VK_U32; c->rvw=0; char *nm=malloc(strlen(e->name)+3);
            snprintf(nm,strlen(e->name)+3,"%%_%s",e->name); return nm; }
        if(r==R_TEXTURE||r==R_SAMPLER){ *k=VK_I32; c->rvw=0; char *nm=malloc(strlen(e->name)+3);
            snprintf(nm,strlen(e->name)+3,"%%_%s",e->name); return nm; }
        if(r==R_CONST){ ConstDef *cd=&c->prog->consts[idx];
            *k = cd->ty.kind==T_BOOL?VK_I1:(cd->ty.kind==T_FLOAT||cd->ty.kind==T_HALF)?VK_F32:(cd->ty.kind==T_UINT32?VK_U32:VK_I32);
            c->rvw=0;
            char ll[16]; ll_of(ll,sizeof ll,cd->ty.kind,0);
            const char *v=newtmp(c);
            emit(c,"  %s = load %s, %s addrspace(2)* @_binc_const_%s, align %d\n",v,ll,ll,cd->name,type_align(cd->ty.kind,0));
            if(cd->ty.kind==T_HALF){ const char *w=newtmp(c); emit(c,"  %s = fpext half %s to float\n",w,v); v=w; }
            return v; }
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
        /* Coordinate properties are built-ins rather than memory fields. The explicit
         * coordinate itself is the global position; local/group values are appended as
         * hidden built-in arguments when referenced. */
        if(e->kind==E_FIELD){
            if(e->operand->kind==E_IDENT){
                int vi; RKind vr=resolve(c,e->operand->name,&vi);
                if(vr==R_SCALAR && c->fn->params[vi].ty.vecn>1){
                    Param *vp=&c->fn->params[vi];
                    int idxs[4]; int nc=swizzle_idx(e->field,idxs);
                    if(nc<0) die(0,"invalid vector component .%s",e->field);
                    for(int i=0;i<nc;i++) if(idxs[i]>=vp->ty.vecn) die(0,"invalid vector component .%s",e->field);
                    char nm[64]; snprintf(nm,sizeof nm,"%%_%s",vp->name);
                    const char *elt = vp->ty.kind==T_HALF?"float":scalar_ll(vp->ty.kind);
                    char vty[32]; snprintf(vty,sizeof vty,"<%d x %s>",vp->ty.vecn,elt);
                    *k=scalar_vk(vp->ty.kind);
                    if(nc==1){ const char *r=newtmp(c); c->rvw=0;
                        emit(c,"  %s = extractelement %s %s, i32 %d\n",r,vty,nm,idxs[0]); return r; }
                    c->rvw=nc;
                    return swizzle_read(c,nm,vty,elt,idxs,nc);
                }
                if(vr==R_LOCAL && c->locs[vi].vecn>1){
                    int idxs[4]; int nc=swizzle_idx(e->field,idxs);
                    if(nc<0) die(0,"invalid vector component .%s",e->field);
                    for(int i=0;i<nc;i++) if(idxs[i]>=c->locs[vi].vecn) die(0,"invalid vector component .%s",e->field);
                    if(nc>1){
                        LInfo li={c->locs[vi].kind,c->locs[vi].sname,0,-1,1,c->locs[vi].vecn,c->locs[vi].matn};
                        ValKind lk; const char *lv=emit_load_t(c,&li,c->locs[vi].slot,&lk);
                        const char *elt = c->locs[vi].kind==T_HALF?"float":scalar_ll(c->locs[vi].kind);
                        char vty[32]; snprintf(vty,sizeof vty,"<%d x %s>",c->locs[vi].vecn,elt);
                        *k=scalar_vk(c->locs[vi].kind); c->rvw=nc;
                        return swizzle_read(c,lv,vty,elt,idxs,nc);
                    }
                }
            }
            /* general multi-component swizzle on any vector lvalue (e.g. verts[vid].xy) */
            {
                int idxs[4]; int nc=swizzle_idx(e->field,idxs);
                if(nc>1){
                    LInfo li; char *addr=gen_lval(c,e->operand,&li,0);
                    if(li.vecn<=1) die(0,"swizzle on a non-vector value");
                    for(int i=0;i<nc;i++) if(idxs[i]>=li.vecn) die(0,"invalid vector component .%s",e->field);
                    ValKind lk; const char *lv=emit_load_t(c,&li,addr,&lk);
                    const char *elt = li.tk==T_HALF?"float":scalar_ll(li.tk);
                    char vty[32]; snprintf(vty,sizeof vty,"<%d x %s>",li.vecn,elt);
                    *k=scalar_vk(li.tk); c->rvw=nc;
                    return swizzle_read(c,lv,vty,elt,idxs,nc);
                }
            }
            if(e->operand->kind==E_IDENT){
            int ci; RKind cr=resolve(c,e->operand->name,&ci);
            if(cr==R_COORD && (!strcmp(e->field,"global")||!strcmp(e->field,"local")||!strcmp(e->field,"group"))){
                Param *cp=&c->fn->params[ci]; char nm[64];
                if(!strcmp(e->field,"global")){ snprintf(nm,sizeof nm,"%%_%s",cp->name); }
                else if(!strcmp(e->field,"local")){ c->uses_local_coord=1; snprintf(nm,sizeof nm,"%%_%s_local",cp->name); }
                else { c->uses_group_coord=1; snprintf(nm,sizeof nm,"%%_%s_group",cp->name); }
                *k=VK_U32; c->rvw=coord_width(&cp->ty); return coord_value(c,&cp->ty,nm);
            }
            }
            if(e->operand->kind==E_FIELD && e->operand->operand->kind==E_IDENT){
                int ci; RKind cr=resolve(c,e->operand->operand->name,&ci);
                if(cr==R_COORD && (!strcmp(e->operand->field,"global")||!strcmp(e->operand->field,"local")||!strcmp(e->operand->field,"group"))){
                    Param *cp=&c->fn->params[ci]; char nm[64];
                    if(!strcmp(e->operand->field,"global")) snprintf(nm,sizeof nm,"%%_%s",cp->name);
                    else if(!strcmp(e->operand->field,"local")){ c->uses_local_coord=1; snprintf(nm,sizeof nm,"%%_%s_local",cp->name); }
                    else { c->uses_group_coord=1; snprintf(nm,sizeof nm,"%%_%s_group",cp->name); }
                    if(cp->ty.coordn==1) die(0,"scalar coordinate has no .%s component",e->field);
                    int x=comp_idx(e->field); if(x<0||x>=cp->ty.coordn) die(0,"invalid coordinate component .%s",e->field);
                    const char *r=newtmp(c); emit(c,"  %s = extractelement <%d x i16> %s, i32 %d\n",r,cp->ty.coordn,nm,x);
                    const char *z=newtmp(c); emit(c,"  %s = zext i16 %s to i32\n",z,r); *k=VK_U32; c->rvw=0; return z;
                }
            }
        }
        LInfo li; char *addr=gen_lval(c,e,&li,0);
        if(li.pi>=0) c->read[li.pi]=1;
        return emit_load_t(c,&li,addr,k);
    }
    case E_NEG:{ ValKind lk; const char *v=gen_rval(c,e->operand,&lk); int vw=c->rvw; const char *r=newtmp(c);
        char ty[32]; ll_of(ty,sizeof ty,lk==VK_F32?T_FLOAT:T_INT32,vw);
        if(lk==VK_F32){ *k=VK_F32; emit(c,"  %s = fneg fast %s %s\n",r,ty,v); }
        else { *k=lk; emit(c,"  %s = sub %s %s, %s\n",r,ty,vw?"zeroinitializer":"0",v); }
        c->rvw=vw; return r; }
    case E_COMPL:{ ValKind lk; const char *v=gen_rval(c,e->operand,&lk); int vw=c->rvw;
        if(lk==VK_F32) die(0,"bitwise complement requires an integer operand");
        char ty[32]; ll_of(ty,sizeof ty,T_INT32,vw);
        const char *r=newtmp(c); *k=lk; c->rvw=vw;
        if(vw){ const char *ones=splat(c,"-1","i32",vw);
            emit(c,"  %s = xor %s %s, %s\n",r,ty,v,ones); }
        else emit(c,"  %s = xor %s %s, -1\n",r,ty,v);
        return r; }
    case E_CAST:{ ValKind lk; const char *v=gen_rval(c,e->operand,&lk); int vw=c->rvw;
        Type *t=&e->cty; TypeKind tk=t->kind; int tv=t->vecn>1?t->vecn:0;
        if(tk==T_BOOL){
            if(vw) die(0,"cannot cast a vector to bool");
            const char *r=newtmp(c); *k=VK_I1; c->rvw=0;
            if(lk==VK_F32) emit(c,"  %s = fcmp one float %s, 0.000000e+00\n",r,v);
            else emit(c,"  %s = icmp ne i32 %s, 0\n",r,v);
            return r;
        }
        ValKind want=scalar_vk(tk);
        if(tv){ /* to vector: same-width conversion, or scalar splat */
            if(vw&&vw!=tv) die(0,"vector width mismatch in cast");
            if(!vw){ const char *s=coerce(c,v,lk,want); *k=want; c->rvw=tv; return splat(c,s,scalar_ll(tk),tv); }
            if(lk!=want) return vconv(c,v,tv,lk,want);
            *k=want; c->rvw=tv; return v;
        }
        if(vw) die(0,"cannot cast a vector to a scalar");
        const char *r=coerce(c,v,lk,want); *k=want; c->rvw=0; return r; }
    case E_NOT:{ ValKind lk; const char *v=gen_rval(c,e->operand,&lk); if(lk!=VK_I1) die(0,"! on non-bool");
        const char *r=newtmp(c); *k=VK_I1; emit(c,"  %s = xor i1 %s, true\n",r,v); return r; }
    case E_BIN:{ ValKind lk,rk; const char *l=gen_rval(c,e->lhs,&lk); int lw=c->rvw; int lm=c->rmat;
        const char *r=gen_rval(c,e->rhs,&rk); int rw=c->rvw; int rm=c->rmat;
        /* ---- matrix arithmetic ---- */
        if(lm&&rm){
            if(lm!=rm) die(0,"matrix width mismatch");
            int mn=lm;
            if(e->bop==B_MUL){
                /* c_out[c][r] = sum_k a[k][r]*b[c][k] */
                const char *res="undef";
                for(int cc=0;cc<mn;cc++){
                    const char *colv="undef";
                    for(int rr=0;rr<mn;rr++){
                        const char *acc=NULL;
                        for(int kk=0;kk<mn;kk++){
                            const char *ae=mat_elem(c,l,mn,kk,rr);
                            const char *be=mat_elem(c,r,mn,cc,kk);
                            const char *p=newtmp(c);
                            emit(c,"  %s = fmul fast float %s, %s\n",p,ae,be);
                            if(!acc) acc=p;
                            else { const char *s=newtmp(c); emit(c,"  %s = fadd fast float %s, %s\n",s,acc,p); acc=s; }
                        }
                        const char *ins=newtmp(c);
                        emit(c,"  %s = insertelement <%d x float> %s, float %s, i32 %d\n",ins,mn,colv,acc,rr);
                        colv=ins;
                    }
                    res=mat_setcol(c,res,mn,cc,colv);
                }
                *k=VK_F32; c->rvw=0; c->rmat=mn; return res;
            }
            if(e->bop==B_ADD||e->bop==B_SUB){
                const char *op=e->bop==B_ADD?"fadd fast":"fsub fast";
                const char *res="undef";
                for(int cc=0;cc<mn;cc++){
                    const char *ac=mat_col(c,l,mn,cc);
                    const char *bc=mat_col(c,r,mn,cc);
                    const char *colv=newtmp(c);
                    emit(c,"  %s = %s <%d x float> %s, %s\n",colv,op,mn,ac,bc);
                    res=mat_setcol(c,res,mn,cc,colv);
                }
                *k=VK_F32; c->rvw=0; c->rmat=mn; return res;
            }
            die(0,"unsupported matrix operation");
        }
        if(lm&&!rm){
            int mn=lm;
            if(e->bop==B_MUL&&rw==mn){
                /* matrix * column vector */
                const char *res="undef";
                for(int rr=0;rr<mn;rr++){
                    const char *acc=NULL;
                    for(int cc=0;cc<mn;cc++){
                        const char *me=mat_elem(c,l,mn,cc,rr);
                        const char *ve=newtmp(c);
                        emit(c,"  %s = extractelement <%d x float> %s, i32 %d\n",ve,mn,r,cc);
                        const char *p=newtmp(c);
                        emit(c,"  %s = fmul fast float %s, %s\n",p,me,ve);
                        if(!acc) acc=p;
                        else { const char *s=newtmp(c); emit(c,"  %s = fadd fast float %s, %s\n",s,acc,p); acc=s; }
                    }
                    const char *ins=newtmp(c);
                    emit(c,"  %s = insertelement <%d x float> %s, float %s, i32 %d\n",ins,mn,res,acc,rr);
                    res=ins;
                }
                *k=VK_F32; c->rvw=mn; c->rmat=0; return res;
            }
            if(e->bop==B_MUL&&!rw){
                /* matrix * scalar */
                const char *s2=splat(c,r,"float",mn);
                const char *res="undef";
                for(int cc=0;cc<mn;cc++){
                    const char *ac=mat_col(c,l,mn,cc);
                    const char *colv=newtmp(c);
                    emit(c,"  %s = fmul fast <%d x float> %s, %s\n",colv,mn,ac,s2);
                    res=mat_setcol(c,res,mn,cc,colv);
                }
                *k=VK_F32; c->rvw=0; c->rmat=mn; return res;
            }
            if(e->bop==B_ADD||e->bop==B_SUB){
                /* matrix +- scalar (elementwise) */
                const char *op=e->bop==B_ADD?"fadd fast":"fsub fast";
                const char *s2=splat(c,r,"float",mn);
                const char *res="undef";
                for(int cc=0;cc<mn;cc++){
                    const char *ac=mat_col(c,l,mn,cc);
                    const char *colv=newtmp(c);
                    emit(c,"  %s = %s <%d x float> %s, %s\n",colv,op,mn,ac,s2);
                    res=mat_setcol(c,res,mn,cc,colv);
                }
                *k=VK_F32; c->rvw=0; c->rmat=mn; return res;
            }
            die(0,"unsupported matrix operation");
        }
        if(!lm&&rm){
            int mn=rm;
            if(e->bop==B_MUL&&lw==mn){
                /* row vector * matrix */
                const char *res="undef";
                for(int rr=0;rr<mn;rr++){
                    const char *acc=NULL;
                    for(int cc=0;cc<mn;cc++){
                        const char *ve=newtmp(c);
                        emit(c,"  %s = extractelement <%d x float> %s, i32 %d\n",ve,mn,l,cc);
                        const char *me=mat_elem(c,r,mn,cc,rr);
                        const char *p=newtmp(c);
                        emit(c,"  %s = fmul fast float %s, %s\n",p,ve,me);
                        if(!acc) acc=p;
                        else { const char *s=newtmp(c); emit(c,"  %s = fadd fast float %s, %s\n",s,acc,p); acc=s; }
                    }
                    const char *ins=newtmp(c);
                    emit(c,"  %s = insertelement <%d x float> %s, float %s, i32 %d\n",ins,mn,res,acc,rr);
                    res=ins;
                }
                *k=VK_F32; c->rvw=mn; c->rmat=0; return res;
            }
            if(e->bop==B_MUL&&!lw){
                /* scalar * matrix */
                const char *s2=splat(c,l,"float",mn);
                const char *res="undef";
                for(int cc=0;cc<mn;cc++){
                    const char *ac=mat_col(c,r,mn,cc);
                    const char *colv=newtmp(c);
                    emit(c,"  %s = fmul fast <%d x float> %s, %s\n",colv,mn,ac,s2);
                    res=mat_setcol(c,res,mn,cc,colv);
                }
                *k=VK_F32; c->rvw=0; c->rmat=mn; return res;
            }
            die(0,"unsupported matrix operation");
        }
        int isf=(lk==VK_F32||rk==VK_F32);
        if(isf&&bitwise_bin(e->bop)) die(0,"bitwise operators require integer operands");
        int uns=(lk==VK_U32||rk==VK_U32);
        ValKind ok=isf?VK_F32:uns?VK_U32:VK_I32;
        int vw=lw?lw:rw;
        if(lw&&rw&&lw!=rw) die(0,"vector width mismatch");
        const char *op=bin_op(e->bop,isf,uns);
        if(vw){ const char *elt=ok==VK_F32?"float":"i32";
            if(!lw){ l=coerce(c,l,lk,ok); l=splat(c,l,elt,vw); } else if(lk!=ok) l=vconv(c,l,lw,lk,ok);
            if(!rw){ r=coerce(c,r,rk,ok); r=splat(c,r,elt,vw); } else if(rk!=ok) r=vconv(c,r,rw,rk,ok);
            const char *v=newtmp(c); *k=ok; c->rvw=vw;
            emit(c,"  %s = %s <%d x %s> %s, %s\n",v,op,vw,elt,l,r); return v; }
        if(isf){ l=coerce(c,l,lk,VK_F32); r=coerce(c,r,rk,VK_F32); }
        const char *v=newtmp(c); *k=ok;
        emit(c,"  %s = %s %s %s, %s\n",v,op,isf?"float":"i32",l,r); return v; }
    case E_CMP:{ ValKind lk,rk; const char *l=gen_rval(c,e->lhs,&lk); int lw=c->rvw;
        const char *r=gen_rval(c,e->rhs,&rk); int rw=c->rvw;
        int vw=lw?lw:rw;
        if(lw&&rw&&lw!=rw) die(0,"vector width mismatch in comparison");
        int isf=(lk==VK_F32||rk==VK_F32);
        int uns=(lk==VK_U32||rk==VK_U32);
        if(vw){
            /* vector comparison: <n x i1> mask, usable with select() */
            const char *elt=isf?"float":"i32";
            ValKind want=isf?VK_F32:(uns?VK_U32:VK_I32);
            if(!lw){ l=coerce(c,l,lk,want); l=splat(c,l,elt,vw); } else if(lk!=want) l=vconv(c,l,vw,lk,want);
            if(!rw){ r=coerce(c,r,rk,want); r=splat(c,r,elt,vw); } else if(rk!=want) r=vconv(c,r,vw,rk,want);
            const char *v=newtmp(c); *k=VK_I1; c->rvw=vw;
            emit(c,"  %s = %s %s <%d x %s> %s, %s\n",v,isf?"fcmp":"icmp",cmp_name(e->cmp,isf,uns),vw,elt,l,r);
            return v;
        }
        if(isf){ l=coerce(c,l,lk,VK_F32); r=coerce(c,r,rk,VK_F32); }
        const char *v=newtmp(c); *k=VK_I1;
        emit(c,"  %s = %s %s %s %s, %s\n",v,isf?"fcmp":"icmp",cmp_name(e->cmp,isf,uns),isf?"float":"i32",l,r); return v; }
    case E_LOG:{ ValKind lk,rk; const char *l=gen_rval(c,e->lhs,&lk); int lw=c->rvw;
        const char *r=gen_rval(c,e->rhs,&rk); if(lw||c->rvw) die(0,"vector operand in logical operator");
        const char *v=newtmp(c); *k=VK_I1; emit(c,"  %s = %s i1 %s, %s\n",v,e->log==L_AND?"and":"or",l,r); return v; }
    case E_ASSIGN:{
        /* swizzle assignment: v.xy = rhs — write each named component */
        if(e->aop==A_ASSIGN && e->operand->kind==E_FIELD && e->operand->operand->kind==E_IDENT){
            int wi; RKind wr=resolve(c,e->operand->operand->name,&wi);
            if(wr==R_LOCAL && c->locs[wi].vecn>1){
                int idxs[4]; int nc=swizzle_idx(e->operand->field,idxs);
                if(nc>1){
                    for(int i=0;i<nc;i++) if(idxs[i]>=c->locs[wi].vecn) die(0,"invalid vector component .%s",e->operand->field);
                    ValKind rk; const char *rv=gen_rval(c,e->rhs,&rk); int rw=c->rvw;
                    if(rw!=nc) die(0,"swizzle assignment width mismatch");
                    char pty[96]; pty_str(pty,sizeof pty,c->locs[wi].kind,NULL,0,1,c->locs[wi].vecn,0);
                    char *base=c->locs[wi].slot;
                    char *bit=newtmp(c);
                    emit(c,"  %s = bitcast %s %s to %s*\n",bit,pty,base,c->locs[wi].kind==T_HALF?"half":"float");
                    for(int i=0;i<nc;i++){
                        const char *p=newtmp(c);
                        emit(c,"  %s = getelementptr inbounds %s, %s* %s, i64 %d\n",p,c->locs[wi].kind==T_HALF?"half":"float",c->locs[wi].kind==T_HALF?"half":"float",bit,idxs[i]);
                        const char *ev=newtmp(c);
                        emit(c,"  %s = extractelement <%d x float> %s, i32 %d\n",ev,nc,rv,i);
                        const char *sv=ev;
                        if(c->locs[wi].kind==T_HALF){ const char *h=newtmp(c); emit(c,"  %s = fptrunc float %s to half\n",h,ev); sv=h; }
                        emit(c,"  store %s %s, %s* %s, align %d\n",c->locs[wi].kind==T_HALF?"half":"float",sv,c->locs[wi].kind==T_HALF?"half":"float",p,c->locs[wi].kind==T_HALF?2:4);
                    }
                    *k=scalar_vk(c->locs[wi].kind); c->rvw=nc; return rv;
                }
            }
        }
        ValKind rk; const char *rhs=gen_rval(c,e->rhs,&rk); int rw=c->rvw; int rm=c->rmat;
        LInfo li; char *addr=gen_lval(c,e->operand,&li,1);
        if(rm){
            if(e->aop!=A_ASSIGN) die(0,"compound assignment on a matrix");
            if(li.matn!=rm) die(0,"matrix width mismatch in assignment");
            char mty[32]; mll_of(mty,sizeof mty,rm);
            emit(c,"  store %s %s, %s %s, align %d\n",mty,rhs,mty,addr,mat_align(rm));
            *k=VK_F32; c->rvw=0; c->rmat=rm; return rhs;
        }
        if(li.tk==T_STRUCT) die(0,"cannot assign a whole struct");
        int vw=li.vecn;
        if(e->aop!=A_ASSIGN){
            ValKind ck; const char *cur=emit_load_t(c,&li,addr,&ck);
            if(li.pi>=0) c->read[li.pi]=1;
            int isf=(ck==VK_F32||rk==VK_F32);
            if(isf&&bitwise_assign(e->aop)) die(0,"bitwise assignment requires integer operands");
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
        warn_implicit(c,e->rhs,rk,li.tk,vw);
        if(vw){ sv=to_storage(c,rhs,rk,rw,li.tk,vw); ev=sv; }
        else { if(rw) die(0,"cannot store a vector into a scalar"); sv=store_val(c,rhs,rk,li.tk,&ev); }
        char pty[96]; pty_str(pty,sizeof pty,li.tk,li.sname,li.as,li.is_local,vw,li.matn);
        char ll[32]; ll_of(ll,sizeof ll,li.tk,vw);
        emit(c,"  store %s %s, %s %s, align %d\n",ll,sv,pty,addr,type_align(li.tk,vw));
        *k=scalar_vk(li.tk); c->rvw=vw; return ev;
    }
    case E_TERNARY:{
        const char *cv=gen_cond(c,e->operand);
        ValKind ak; const char *av=gen_rval(c,e->lhs,&ak); int aw=c->rvw;
        ValKind bk; const char *bv=gen_rval(c,e->rhs,&bk); int bw=c->rvw;
        int vw=aw?aw:bw;
        if(aw&&bw&&aw!=bw) die(0,"vector width mismatch in ternary");
        int isf=(ak==VK_F32||bk==VK_F32);
        ValKind ok=isf?VK_F32:((ak==VK_U32||bk==VK_U32)?VK_U32:VK_I32);
        av=coerce(c,av,ak,ok); bv=coerce(c,bv,bk,ok);
        const char *elt=ok==VK_F32?"float":"i32";
        const char *r=newtmp(c); *k=ok; c->rvw=vw;
        if(vw){ if(!aw) av=splat(c,av,elt,vw); if(!bw) bv=splat(c,bv,elt,vw);
            const char *mask=splat(c,cv,"i1",vw);
            char ty[32], mty[32]; ll_of(ty,sizeof ty,ok==VK_F32?T_FLOAT:T_INT32,vw);
            snprintf(mty,sizeof mty,"<%d x i1>",vw);
            emit(c,"  %s = select %s %s, %s %s, %s %s\n",r,mty,mask,ty,av,ty,bv); }
        else emit(c,"  %s = select i1 %s, %s %s, %s %s\n",r,cv,elt,av,elt,bv);
        return r; }
    case E_CALL:{
        if(e->callee && e->callee->kind==E_FIELD){
            /* texture methods: tex.read(c), tex.write(v, c), tex.sample(smp, uv) */
            if(e->callee->operand->kind==E_IDENT){
                int ti; RKind tr=resolve(c,e->callee->operand->name,&ti);
                if(tr==R_TEXTURE){
                    Param *tp=&c->fn->params[ti];
                    const char *elt,*vec,*suf,*an; tex_kinds(tp->ty.tex_elt,&elt,&vec,&suf,&an);
                    (void)elt; (void)an;
                    char tname[64]; snprintf(tname,sizeof tname,"%%_%s",tp->name);
                    if(!strcmp(e->name,"read")){
                        if(e->nargs!=1) die(0,"texture read expects 1 argument (the coordinate)");
                        ValKind ck; const char *cv=gen_rval(c,e->args[0],&ck);
                        if(c->rvw!=2) die(0,"texture read coordinate must be an int2");
                        get_samp_used=1; tex_read_used[tp->ty.tex_elt==T_HALF?1:tp->ty.tex_elt==T_INT32?2:tp->ty.tex_elt==T_UINT32?3:0]=1;
                        const char *samp=newtmp(c);
                        emit(c,"  %s = call %%struct._sampler_t addrspace(2)* @air.get_read_sampler()\n",samp);
                        const char *r=newtmp(c);
                        emit(c,"  %s = call { %s, i8 } @air.read_texture_2d.%s(%%struct._texture_2d_t addrspace(1)* %s, %%struct._sampler_t addrspace(2)* %s, <2 x i32> %s, <2 x i32> zeroinitializer, i32 0, i32 0)\n",r,vec,suf,tname,samp,cv);
                        const char *v=newtmp(c);
                        emit(c,"  %s = extractvalue { %s, i8 } %s, 0\n",v,vec,r);
                        if(tp->ty.tex_elt==T_HALF){ const char *w=newtmp(c); emit(c,"  %s = fpext <4 x half> %s to <4 x float>\n",w,v); v=w; }
                        *k=VK_F32; c->rvw=4; return v;
                    }
                    if(!strcmp(e->name,"write")){
                        if(e->nargs!=2) die(0,"texture write expects 2 arguments (value, coordinate)");
                        ValKind vk; const char *vv=gen_rval(c,e->args[0],&vk); int vw=c->rvw;
                        ValKind ck; const char *cv=gen_rval(c,e->args[1],&ck);
                        if(vw!=4) die(0,"texture write value must be a float4");
                        if(c->rvw!=2) die(0,"texture write coordinate must be an int2");
                        tex_write_used[tp->ty.tex_elt==T_HALF?1:tp->ty.tex_elt==T_INT32?2:tp->ty.tex_elt==T_UINT32?3:0]=1;
                        const char *sv=vv;
                        if(tp->ty.tex_elt==T_HALF){ const char *h=newtmp(c); emit(c,"  %s = fptrunc <4 x float> %s to <4 x half>\n",h,vv); sv=h; }
                        else if(tp->ty.tex_elt==T_INT32||tp->ty.tex_elt==T_UINT32) sv=vconv(c,vv,4,VK_F32,tp->ty.tex_elt==T_INT32?VK_I32:VK_U32);
                        emit(c,"  call void @air.write_texture_2d.%s(%%struct._texture_2d_t addrspace(1)* %s, <2 x i32> %s, %s %s, i32 0, i32 2)\n",suf,tname,cv,vec,sv);
                        *k=VK_I32; c->rvw=0; return "0";
                    }
                    if(!strcmp(e->name,"sample")){
                        if(e->nargs!=2) die(0,"texture sample expects 2 arguments (sampler, uv)");
                        int si; if(e->args[0]->kind!=E_IDENT||resolve(c,e->args[0]->name,&si)!=R_SAMPLER)
                            die(0,"texture sample's first argument must be a sampler parameter");
                        ValKind uk; const char *uv=gen_rval(c,e->args[1],&uk);
                        if(c->rvw!=2) die(0,"texture sample uv must be a float2");
                        tex_sample_used[tp->ty.tex_elt==T_HALF?1:tp->ty.tex_elt==T_INT32?2:tp->ty.tex_elt==T_UINT32?3:0]=1;
                        char sname[64]; snprintf(sname,sizeof sname,"%%_%s",e->args[0]->name);
                        const char *r=newtmp(c);
                        emit(c,"  %s = call { %s, i8 } @air.sample_texture_2d.%s(%%struct._texture_2d_t addrspace(1)* %s, %%struct._sampler_t addrspace(2)* %s, <2 x float> %s, i1 true, <2 x i32> zeroinitializer, i1 false, float %s, float %s, i32 0)\n",r,vec,suf,tname,sname,uv,fconst(c,0.0),fconst(c,0.0));
                        const char *v=newtmp(c);
                        emit(c,"  %s = extractvalue { %s, i8 } %s, 0\n",v,vec,r);
                        if(tp->ty.tex_elt==T_HALF){ const char *w=newtmp(c); emit(c,"  %s = fpext <4 x half> %s to <4 x float>\n",w,v); v=w; }
                        *k=VK_F32; c->rvw=4; return v;
                    }
                    die(0,"unknown texture method .%s (use read, write, sample)",e->name);
                }
            }
            if(strcmp(e->name,"add")) die(0,"unsupported atomic method %s",e->name);
            if(e->nargs!=1) die(0,"atomic add expects one argument");
            Expr *base=e->callee->operand; if(base->kind!=E_DEREF||base->operand->kind!=E_IDENT) die(0,"atomic methods require an atomic buffer");
            int api; if(resolve(c,base->operand->name,&api)!=R_PTR||c->fn->params[api].ty.kind!=T_ATOMIC) die(0,".add() target is not atomic");
            LInfo ali; fill_param_li(c,api,&ali); ali.pi=api; c->read[api]=1; c->written[api]=1;
            char *ap=element_ptr_idx(c,api,"0"); TypeKind ak=c->fn->params[api].ty.atomic_base; ValKind vk; const char *v=gen_rval(c,e->args[0],&vk);
            if(c->rvw) die(0,"atomic add does not accept vectors");
            ValKind want=scalar_vk(ak); v=coerce(c,v,vk,want);
            const char *intr=ak==T_FLOAT?"air.atomic.global.add.f32":"air.atomic.global.add.i32";
            char *pp=newtmp(c); emit(c,"  %s = getelementptr inbounds %%\"struct.metal::_atomic\", %%\"struct.metal::_atomic\" addrspace(1)* %s, i64 0, i32 0\n",pp,ap);
            const char *r=newtmp(c); emit(c,"  %s = call %s @%s(%s addrspace(1)* %s, %s %s, i32 0, i32 2, i32 0, i1 false)\n",r,scalar_ll(ak),intr,scalar_ll(ak),pp,scalar_ll(ak),v);
            atomic_add_used[ak==T_FLOAT?0:1]=1; *k=want; c->rvw=0; return r;
        }
        /* matrix constructor mat2/3/4(...) */
        {
            int mn=0;
            if(!strcmp(e->name,"mat2")) mn=2; else if(!strcmp(e->name,"mat3")) mn=3; else if(!strcmp(e->name,"mat4")) mn=4;
            if(mn){
                if((int)e->nargs==1){
                    /* diagonal matrix from one scalar */
                    ValKind ak; const char *av=gen_rval(c,e->args[0],&ak);
                    if(c->rvw) die(0,"mat%d: single-argument form takes a scalar",mn);
                    const char *res="undef";
                    for(int cc=0;cc<mn;cc++){
                        const char *colv="undef";
                        for(int rr=0;rr<mn;rr++){
                            const char *val=(rr==cc)?av:fconst(c,0.0);
                            const char *ins=newtmp(c);
                            emit(c,"  %s = insertelement <%d x float> %s, float %s, i32 %d\n",ins,mn,colv,val,rr);
                            colv=ins;
                        }
                        res=mat_setcol(c,res,mn,cc,colv);
                    }
                    *k=VK_F32; c->rvw=0; c->rmat=mn; return res;
                }
                if((int)e->nargs==mn){
                    /* column vectors */
                    ValKind aks[4]; const char *cols[4]; int ok=1;
                    for(int i=0;i<mn;i++){ cols[i]=gen_rval(c,e->args[i],&aks[i]); if(c->rvw!=mn) ok=0; }
                    if(!ok) die(0,"mat%d: column arguments must be float%d vectors",mn,mn);
                    const char *res="undef";
                    for(int cc=0;cc<mn;cc++) res=mat_setcol(c,res,mn,cc,cols[cc]);
                    *k=VK_F32; c->rvw=0; c->rmat=mn; return res;
                }
                if((int)e->nargs==mn*mn){
                    /* scalars, column-major */
                    ValKind aks[16]; const char *svals[16];
                    for(int i=0;i<mn*mn;i++){ svals[i]=gen_rval(c,e->args[i],&aks[i]); if(c->rvw) die(0,"mat%d: scalar arguments must be scalars",mn); }
                    const char *res="undef";
                    for(int cc=0;cc<mn;cc++){
                        const char *colv="undef";
                        for(int rr=0;rr<mn;rr++){
                            const char *ins=newtmp(c);
                            emit(c,"  %s = insertelement <%d x float> %s, float %s, i32 %d\n",ins,mn,colv,svals[cc*mn+rr],rr);
                            colv=ins;
                        }
                        res=mat_setcol(c,res,mn,cc,colv);
                    }
                    *k=VK_F32; c->rvw=0; c->rmat=mn; return res;
                }
                die(0,"mat%d expects 1 scalar, %d scalars, or %d column vectors",mn,mn,mn);
            }
        }
        /* vector constructor? float4(...)/int3(...)/uint2(...): 1 scalar (splat),
         * N scalars, or mixed scalars/vectors totalling N components (float3(v2, s), ...) */
        TypeKind cb; int cn;
        if(vec_name(e->name,&cb,&cn)){
            if(e->nargs!=1&&(int)e->nargs>cn) die(0,"%s expects 1 to %d argument(s)",e->name,cn);
            const char *elt=scalar_ll(cb); const char *acc="undef";
            if((int)e->nargs==1){
                ValKind ak; const char *v=gen_rval(c,e->args[0],&ak);
                if(c->rvw) die(0,"vector argument in %s constructor",e->name);
                v=coerce(c,v,ak,scalar_vk(cb));
                for(int i=0;i<cn;i++){ const char *r=newtmp(c);
                    emit(c,"  %s = insertelement <%d x %s> %s, %s %s, i32 %d\n",r,cn,elt,acc,elt,v,i); acc=r; }
                *k=scalar_vk(cb); c->rvw=cn; return acc;
            }
            /* N scalars or mixed scalars+vectors: total components must equal cn */
            int comp=0;
            for(size_t i=0;i<e->nargs;i++){
                ValKind ak; const char *v=gen_rval(c,e->args[i],&ak); int w=c->rvw;
                if(!w){
                    v=coerce(c,v,ak,scalar_vk(cb));
                    const char *r=newtmp(c);
                    emit(c,"  %s = insertelement <%d x %s> %s, %s %s, i32 %d\n",r,cn,elt,acc,elt,v,comp); acc=r; comp++;
                } else {
                    if(w+comp>cn) die(0,"%s: argument %zu has too many components",e->name,i+1);
                    const char *sv=v;
                    for(int j=0;j<w;j++){
                        const char *ev=newtmp(c);
                        emit(c,"  %s = extractelement <%d x %s> %s, i32 %d\n",ev,w,scalar_ll(ak==VK_F32?T_FLOAT:ak==VK_U32?T_UINT32:T_INT32),sv,j);
                        const char *cv2=coerce(c,ev,ak,scalar_vk(cb));
                        const char *r=newtmp(c);
                        emit(c,"  %s = insertelement <%d x %s> %s, %s %s, i32 %d\n",r,cn,elt,acc,elt,cv2,comp); acc=r; comp++;
                    }
                }
            }
            if(comp!=cn) die(0,"%s: components total %d, expected %d",e->name,comp,cn);
            *k=scalar_vk(cb); c->rvw=cn; return acc;
        }
        /* select(a, b, mask): per-element pick, scalar or vector */
        if(!strcmp(e->name,"select")){
            if(e->nargs!=3) die(0,"select expects 3 arguments");
            ValKind ak; const char *av=gen_rval(c,e->args[0],&ak); int aw=c->rvw;
            ValKind bk; const char *bv=gen_rval(c,e->args[1],&bk); int bw=c->rvw;
            ValKind mk; const char *mv=gen_rval(c,e->args[2],&mk); int mw=c->rvw;
            if(mk!=VK_I1) die(0,"select mask must be a bool");
            int vw=aw?aw:(bw?bw:mw);
            if((aw&&bw&&aw!=bw)||(aw&&mw&&aw!=mw)||(bw&&mw&&bw!=mw)) die(0,"select vector width mismatch");
            int isf=(ak==VK_F32||bk==VK_F32);
            ValKind ok=isf?VK_F32:((ak==VK_U32||bk==VK_U32)?VK_U32:VK_I32);
            av=coerce(c,av,ak,ok); bv=coerce(c,bv,bk,ok);
            const char *elt=ok==VK_F32?"float":"i32";
            const char *r=newtmp(c); *k=ok; c->rvw=vw;
            if(vw){
                if(!aw) av=splat(c,av,elt,vw); if(!bw) bv=splat(c,bv,elt,vw);
                if(!mw) mv=splat(c,mv,"i1",vw);
                char ty[32], mty[32]; ll_of(ty,sizeof ty,ok==VK_F32?T_FLOAT:T_INT32,vw);
                snprintf(mty,sizeof mty,"<%d x i1>",vw);
                emit(c,"  %s = select %s %s, %s %s, %s %s\n",r,mty,mv,ty,av,ty,bv);
            } else emit(c,"  %s = select i1 %s, %s %s, %s %s\n",r,mv,elt,av,elt,bv);
            return r;
        }
        /* composite math builtins (dot, cross, length, clamp, mix, ...) */
        { const char *cm=gen_composite_math(c,e,k); if(cm) return cm; }
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
                    if(cp->ty.kind!=p->ty.kind||cp->ty.as!=p->ty.as||cp->ty.vecn!=p->ty.vecn||cp->ty.matn!=p->ty.matn)
                        die(0,"%s: pointer argument %d type/address-space mismatch",e->name,(int)i+1);
                    if(p->ty.kind==T_STRUCT && strcmp(cp->ty.struct_name,p->ty.struct_name))
                        die(0,"%s: pointer argument %d struct type mismatch",e->name,(int)i+1);
                    char elt[64]; if(p->ty.kind==T_STRUCT)snprintf(elt,sizeof elt,"%%struct.%s",p->ty.struct_name);
                        else ll_of(elt,sizeof elt,p->ty.kind,p->ty.vecn);
                    o+=snprintf(args+o,sizeof args-o,"%s addrspace(%d)* %%_%s",elt,p->ty.as,cp->name);
                } else { ValKind ak; const char *v=gen_rval(c,e->args[i],&ak);
                    warn_implicit(c,e->args[i],ak,p->ty.kind,p->ty.vecn);
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
            if(bi->ret==T_VOID){
                if(c->divergent) die(0,"sync() is inside divergent control flow; every threadgroup lane must reach the barrier");
                c->uses_sync=1; emit(c,"  call void @%s(i32 2, i32 5, i32 1)\n",bi->ll); *k=VK_I32; return "0"; }
            ValKind aks[4]; const char *vls[4]; int wids[4];
            int vw=0;
            for(int i=0;i<bi->nargs;i++){ vls[i]=gen_rval(c,e->args[i],&aks[i]); wids[i]=c->rvw; if(wids[i]) vw=wids[i]; }
            if(vw && !bi->vec_ok) die(0,"vector argument to builtin %s",e->name);
            for(int i=0;i<bi->nargs;i++) if(wids[i]&&wids[i]!=vw) die(0,"vector width mismatch in %s",e->name);
            if(vw){
                /* per-element application of the scalar intrinsic */
                const char *ret_elt=scalar_ll(bi->ret);
                char rty[32]; snprintf(rty,sizeof rty,"<%d x %s>",vw,ret_elt);
                const char *acc="undef";
                for(int kk=0;kk<vw;kk++){
                    char da[128]; size_t o2=0;
                    for(int i=0;i<bi->nargs;i++){
                        const char *ael=aks[i]==VK_F32?"float":"i32";
                        char vty[32]; snprintf(vty,sizeof vty,"<%d x %s>",vw,ael);
                        const char *x=newtmp(c);
                        emit(c,"  %s = extractelement %s %s, i32 %d\n",x,vty,vls[i],kk);
                        TypeKind at=i==0?bi->a0:bi->a1; const char *ev;
                        const char *sv=store_val(c,x,aks[i],at,&ev);
                        o2+=snprintf(da+o2,sizeof da-o2,"%s%s %s",i?", ":"",scalar_ll(at),sv);
                    }
                    const char *r2=newtmp(c);
                    emit(c,"  %s = call %s @%s(%s)\n",r2,ret_elt,bi->ll,da);
                    const char *ins=newtmp(c);
                    emit(c,"  %s = insertelement %s %s, %s %s, i32 %d\n",ins,rty,acc,ret_elt,r2,kk);
                    acc=ins;
                }
                *k=scalar_vk(bi->ret); c->rvw=vw; return acc;
            }
            char args[256]; size_t o=0;
            for(int i=0;i<bi->nargs;i++){
                TypeKind at=i==0?bi->a0:bi->a1; const char *ev; const char *sv=store_val(c,vls[i],aks[i],at,&ev);
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
    g_last_line=e->line; g_last_col=e->col;
    li->matn=0; /* every path must set vecn/matn explicitly; start clean */
    if(e->kind==E_IDENT){
        int idx; RKind r=resolve(c,e->name,&idx);
        if(r==R_LOCAL){ li->tk=c->locs[idx].kind; li->sname=c->locs[idx].sname; li->as=0; li->pi=-1;
            li->is_local=1; li->vecn=c->locs[idx].vecn; li->matn=c->locs[idx].matn;
            if(mark && c->locs[idx].is_const) die(0,"cannot write to const local %s",e->name);
            return c->locs[idx].slot; }
        die(0,"%s is not a mutable local",e->name);
    }
    if(e->kind==E_DEREF){ const char *ix; int pi=eval_ptr(c,e->operand,&ix);
        if(mark)c->written[pi]=1; fill_param_li(c,pi,li); return element_ptr_idx(c,pi,ix); }
    if(e->kind==E_INDEX){
        /* matrix element m[c][r] (two-level on a matrix base) */
        int mn = e->operand->kind==E_INDEX ? matrix_n(c,e->operand->operand) : 0;
        if(mn){
            ValKind ck, rk;
            const char *cv=gen_rval(c,e->operand->rhs,&ck);
            const char *rv=gen_rval(c,e->rhs,&rk);
            if((ck!=VK_I32&&ck!=VK_U32)||(rk!=VK_I32&&rk!=VK_U32)) die(0,"matrix indices must be integers");
            char *base=gen_lval(c,e->operand->operand,li,mark);
            char mty[32]; mll_of(mty,sizeof mty,mn);
            char *colptr=newtmp(c);
            emit(c,"  %s = getelementptr inbounds %s, %s* %s, i64 0, i64 %s\n",colptr,mty,mty,base,cv);
            char *elt=newtmp(c);
            emit(c,"  %s = getelementptr inbounds <%d x float>, <%d x float>* %s, i64 0, i64 %s\n",elt,mn,mn,colptr,rv);
            li->tk=T_FLOAT; li->sname=NULL; li->vecn=0; li->matn=0;
            return elt;
        }
        /* single-level matrix column: m[c] */
        if(e->operand->kind==E_IDENT||e->operand->kind==E_FIELD){
            int mn2=matrix_n(c,e->operand);
            if(mn2){
                ValKind ik; const char *iv=gen_rval(c,e->rhs,&ik);
                if(ik!=VK_I32&&ik!=VK_U32) die(0,"matrix index must be an integer");
                char *base=gen_lval(c,e->operand,li,mark);
                char mty[32]; mll_of(mty,sizeof mty,mn2);
                char *colptr=newtmp(c);
                emit(c,"  %s = getelementptr inbounds %s, %s* %s, i64 0, i64 %s\n",colptr,mty,mty,base,iv);
                li->tk=T_FLOAT; li->sname=NULL; li->vecn=mn2; li->matn=0;
                return colptr;
            }
        }
        int pi; const char *base=NULL;
        /* Threadgroup arrays are module globals. Fold one- and two-dimensional
         * subscripts into a single GEP so their ABI has no fake buffer argument. */
        if(e->operand->kind==E_IDENT){
            if(resolve(c,e->operand->name,&pi)!=R_PTR) die(0,"subscript of non-pointer");
            Param *sp=&c->fn->params[pi];
            if(sp->ty.array_n){
                ValKind ik; const char *iv=gen_rval(c,e->rhs,&ik); if(ik!=VK_I32&&ik!=VK_U32)die(0,"shared-memory subscript must be an integer");
                char gn[128],elt[64]; shared_name(c,pi,gn,sizeof gn); ll_of(elt,sizeof elt,sp->ty.kind,sp->ty.vecn);
                char *p=newtmp(c); if(sp->ty.array_m) emit(c,"  %s = getelementptr inbounds [%d x [%d x %s]], [%d x [%d x %s]] addrspace(3)* %s, i32 0, i32 %s\n",p,sp->ty.array_n,sp->ty.array_m,elt,sp->ty.array_n,sp->ty.array_m,elt,gn,iv);
                else emit(c,"  %s = getelementptr inbounds [%d x %s], [%d x %s] addrspace(3)* %s, i32 0, i32 %s\n",p,sp->ty.array_n,elt,sp->ty.array_n,elt,gn,iv);
                if(sp->ty.array_m){ li->tk=sp->ty.kind; li->sname=NULL; li->as=AS_THREADGROUP; li->pi=-1; li->is_local=0; li->vecn=sp->ty.vecn; return p; }
                li->tk=sp->ty.kind; li->sname=sp->ty.struct_name; li->as=AS_THREADGROUP; li->pi=-1; li->is_local=0; li->vecn=sp->ty.vecn; return p;
            }
        } else if(e->operand->kind==E_INDEX && e->operand->operand->kind==E_IDENT){
            if(resolve(c,e->operand->operand->name,&pi)!=R_PTR) die(0,"subscript of non-pointer");
            Param *sp=&c->fn->params[pi]; if(!sp->ty.array_n||!sp->ty.array_m) die(0,"too many subscripts");
            ValKind a,b; const char *av=gen_rval(c,e->operand->rhs,&a), *bv=gen_rval(c,e->rhs,&b);
            if((a!=VK_I32&&a!=VK_U32)||(b!=VK_I32&&b!=VK_U32))die(0,"shared-memory subscript must be an integer");
            char gn[128],elt[64]; shared_name(c,pi,gn,sizeof gn); ll_of(elt,sizeof elt,sp->ty.kind,sp->ty.vecn); char *p=newtmp(c);
            emit(c,"  %s = getelementptr inbounds [%d x [%d x %s]], [%d x [%d x %s]] addrspace(3)* %s, i32 0, i32 %s, i32 %s\n",p,sp->ty.array_n,sp->ty.array_m,elt,sp->ty.array_n,sp->ty.array_m,elt,gn,av,bv);
            li->tk=sp->ty.kind; li->sname=sp->ty.struct_name; li->as=AS_THREADGROUP; li->pi=-1; li->is_local=0; li->vecn=sp->ty.vecn; return p;
        }
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
            char pty[96]; pty_str(pty,sizeof pty,T_STRUCT,li->sname,li->as,li->is_local,0,0);
            char *fp=newtmp(c);
            emit(c,"  %s = getelementptr inbounds %%struct.%s, %s %s, i64 0, i32 %d\n",fp,li->sname,pty,addr,fi);
            li->tk=s->fields[fi].ty.kind; li->sname=s->fields[fi].ty.struct_name; li->vecn=s->fields[fi].ty.vecn; li->matn=s->fields[fi].ty.matn;
            return fp; }
        if(li->vecn>1){ /* vector component: bitcast the vector address to element pointer + gep */
            int ci=comp_idx(e->field);
            if(ci<0||ci>=li->vecn) die(0,"no component .%s on <%d x %s>",e->field,li->vecn,scalar_ll(li->tk));
            char vpty[96],epty[96];
            pty_str(vpty,sizeof vpty,li->tk,NULL,li->as,li->is_local,li->vecn,li->matn);
            pty_str(epty,sizeof epty,li->tk,NULL,li->as,li->is_local,0,0);
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
    g_last_line=e->line; g_last_col=e->col;
    ValKind k; const char *v=gen_rval(c,e,&k);
    if(c->rvw) die(0,"vector value in a condition");
    if(k==VK_I1) return v;
    const char *r=newtmp(c);
    if(k==VK_F32) emit(c,"  %s = fcmp one float %s, %s\n",r,v,fconst(c,0.0));
    else emit(c,"  %s = icmp ne i32 %s, 0\n",r,v);
    return r;
}
/* divergence heuristic: does expr touch device/constant element data (=> varying)? */
static int is_varying(CG *c, Expr *e){
    if(!e) return 0;
    if(e->kind==E_IDENT){ int i; RKind r=resolve(c,e->name,&i);
        if(r==R_COORD) return 1;
        if(r==R_SCALAR && c->fn->params[i].un==UN_VARYING) return 1;
    }
    if(e->kind==E_DEREF||e->kind==E_FIELD||e->kind==E_INDEX){ if(root_param(c,e)>=0) return 1; }
    if(is_varying(c,e->operand)||is_varying(c,e->lhs)||is_varying(c,e->rhs)) return 1;
    for(size_t i=0;i<e->nargs;i++) if(is_varying(c,e->args[i])) return 1;
    return 0;
}

static void gen_block(CG *c, Block *b);
static void stage_lit_type(char *buf, size_t n, StructDef *sd);
static void gen_stmt(CG *c, Stmt *s){
    g_last_line=s->line; g_last_col=s->col;
    switch(s->kind){
    case S_EXPR:{ ValKind k; if(s->expr) gen_rval(c,s->expr,&k); break; }
    case S_DECL:{ TypeKind kk=s->ty.kind;
        if(kk==T_STRUCT){ StructDef *sd=find_struct(c->prog,s->ty.struct_name);
            if(!sd) die(0,"unknown struct %s",s->ty.struct_name);
            if(s->init) die(0,"struct local initializer not supported; assign fields instead");
            int sal; struct_layout(sd,&sal);
            char *slot=newtmp(c); sb_printf(c->pre,"  %s = alloca %%struct.%s, align %d\n",slot,s->ty.struct_name,sal);
            c->locs=realloc(c->locs,(c->nlocs+1)*sizeof(Loc));
            c->locs[c->nlocs++]=(Loc){s->name,slot,kk,s->ty.struct_name,0,0,0};
            break; }
        if(s->ty.matn){
            char mty[32]; mll_of(mty,sizeof mty,s->ty.matn);
            char *slot=newtmp(c); sb_printf(c->pre,"  %s = alloca %s, align %d\n",slot,mty,mat_align(s->ty.matn));
            c->locs=realloc(c->locs,(c->nlocs+1)*sizeof(Loc));
            c->locs[c->nlocs++]=(Loc){s->name,slot,kk,NULL,0,s->ty.matn,s->is_const};
            if(s->init){ ValKind k; const char *v=gen_rval(c,s->init,&k);
                if(c->rmat!=s->ty.matn) die(0,"matrix width mismatch in initializer");
                emit(c,"  store %s %s, %s* %s, align %d\n",mty,v,mty,slot,mat_align(s->ty.matn)); }
            break; }
        char ll[32]; ll_of(ll,sizeof ll,kk,s->ty.vecn);
        char *slot=newtmp(c); sb_printf(c->pre,"  %s = alloca %s, align %d\n",slot,ll,type_align(kk,s->ty.vecn));
        c->locs=realloc(c->locs,(c->nlocs+1)*sizeof(Loc));
        c->locs[c->nlocs++]=(Loc){s->name,slot,kk,NULL,s->ty.vecn,0,s->is_const};
        if(s->init){ ValKind k; const char *v=gen_rval(c,s->init,&k);
            warn_implicit(c,s->init,k,kk,s->ty.vecn);
            const char *sv=to_storage(c,v,k,c->rvw,kk,s->ty.vecn);
            emit(c,"  store %s %s, %s* %s, align %d\n",ll,sv,ll,slot,type_align(kk,s->ty.vecn)); }
        break; }
    case S_RETURN:{ TypeKind rk=c->fn->ret.kind;
        if(s->expr){ if(rk==T_VOID) die(0,"return with a value in void function");
            if(rk==T_STRUCT){
                /* stage struct return: build the literal output struct from a struct local */
                if(c->fn->stage==ST_NONE) die(0,"struct-by-value return is only supported in vertex/fragment functions");
                if(s->expr->kind!=E_IDENT) die(0,"stage struct returns must name a struct local");
                int li; if(resolve(c,s->expr->name,&li)!=R_LOCAL||c->locs[li].kind!=T_STRUCT)
                    die(0,"stage struct returns must name a struct local");
                StructDef *sd=find_struct(c->prog,c->locs[li].sname);
                int sal; struct_layout(sd,&sal);
                const char *v=newtmp(c);
                emit(c,"  %s = load %%struct.%s, %%struct.%s* %s, align %d\n",v,c->locs[li].sname,c->locs[li].sname,c->locs[li].slot,sal);
                const char *lit="undef";
                for(size_t f=0;f<sd->nfields;f++){
                    const char *ev=newtmp(c);
                    emit(c,"  %s = extractvalue %%struct.%s %s, %zu\n",ev,c->locs[li].sname,v,f);
                    char fl[32]; ll_of(fl,sizeof fl,sd->fields[f].ty.kind,sd->fields[f].ty.vecn);
                    char lt[256]; stage_lit_type(lt,sizeof lt,sd);
                    const char *ins=newtmp(c);
                    emit(c,"  %s = insertvalue %s %s, %s %s, %zu\n",ins,lt,lit,fl,ev,f);
                    lit=ins;
                }
                char lt[256]; stage_lit_type(lt,sizeof lt,sd);
                emit(c,"  ret %s %s\n",lt,lit);
            } else {
                ValKind k; const char *v=gen_rval(c,s->expr,&k);
                const char *sv=to_storage(c,v,k,c->rvw,rk,c->fn->ret.vecn);
                char ll[32]; ll_of(ll,sizeof ll,rk,c->fn->ret.vecn);
                emit(c,"  ret %s %s\n",ll,sv);
            } }
        else { if(rk!=T_VOID) die(0,"return without a value in non-void function");
            emit(c,"  ret void\n"); }
        c->term=1; break; }
    case S_BREAK:{ if(!c->nloops) die(0,"break outside a loop");
        emit(c,"  br label %%bb%d\n",c->brk_l[c->nloops-1]); c->term=1; break; }
    case S_CONTINUE:{ if(!c->nloops) die(0,"continue outside a loop");
        emit(c,"  br label %%bb%d\n",c->cont_l[c->nloops-1]); c->term=1; break; }
    case S_BLOCK:{ gen_block(c,&s->then_b); break; }
    case S_IF:{
        int div=is_varying(c,s->cond);
        if(div) fprintf(stderr,"binc: note (line %d): 'if' condition is data-dependent (divergent branch) — may cost SIMT performance\n",s->cond->line);
        const char *cv=gen_cond(c,s->cond);
        int tl=newlbl(c), fl=newlbl(c), en=newlbl(c);
        int has_else=s->else_b.n>0;
        emit(c,"  br i1 %s, label %%bb%d, label %%bb%d\n",cv,tl,has_else?fl:en);
        lbl(c,tl); if(div)c->divergent++; gen_block(c,&s->then_b); if(div)c->divergent--; if(!c->term) emit(c,"  br label %%bb%d\n",en);
        if(has_else){ lbl(c,fl); if(div)c->divergent++; gen_block(c,&s->else_b); if(div)c->divergent--; if(!c->term) emit(c,"  br label %%bb%d\n",en); }
        lbl(c,en); break; }
    case S_WHILE:{
        int div=is_varying(c,s->cond);
        if(div) fprintf(stderr,"binc: note (line %d): 'while' condition is data-dependent — divergent\n",s->cond->line);
        int cond=newlbl(c), body=newlbl(c), en=newlbl(c);
        emit(c,"  br label %%bb%d\n",cond); lbl(c,cond);
        const char *cv=gen_cond(c,s->cond); emit(c,"  br i1 %s, label %%bb%d, label %%bb%d\n",cv,body,en);
        lbl(c,body);
        c->brk_l[c->nloops]=en; c->cont_l[c->nloops]=cond; c->nloops++;
        if(div)c->divergent++; gen_block(c,&s->then_b); if(div)c->divergent--; c->nloops--;
        if(!c->term) emit(c,"  br label %%bb%d\n",cond); lbl(c,en); break; }
    case S_DOWHILE:{
        int div=s->cond&&is_varying(c,s->cond);
        if(div) fprintf(stderr,"binc: note (line %d): 'do-while' condition is data-dependent — divergent\n",s->cond->line);
        int body=newlbl(c), cond=newlbl(c), en=newlbl(c);
        emit(c,"  br label %%bb%d\n",body); lbl(c,body);
        c->brk_l[c->nloops]=en; c->cont_l[c->nloops]=cond; c->nloops++;
        if(div)c->divergent++; gen_block(c,&s->then_b); if(div)c->divergent--; c->nloops--;
        if(!c->term) emit(c,"  br label %%bb%d\n",cond);
        lbl(c,cond);
        const char *cv=gen_cond(c,s->cond); emit(c,"  br i1 %s, label %%bb%d, label %%bb%d\n",cv,body,en);
        lbl(c,en); break; }
    case S_SWITCH:{
        ValKind ck; const char *cv=gen_rval(c,s->sw_cond,&ck);
        if(c->rvw) die(0,"cannot switch on a vector");
        if(ck==VK_F32) die(0,"switch expression must be an integer");
        int div=is_varying(c,s->sw_cond);
        if(div) fprintf(stderr,"binc: note (line %d): switch expression is data-dependent — divergent dispatch\n",s->sw_cond->line);
        int n=(int)s->ncases;
        int *cl=malloc((n?n:1)*sizeof(int)); for(int i=0;i<n;i++) cl[i]=newlbl(c);
        int dfl=newlbl(c), end=newlbl(c);
        int els=s->has_default?dfl:end;
        if(n==0){
            if(!s->has_default) die(0,"switch with no cases");
            emit(c,"  br label %%bb%d\n",dfl);
        } else {
            int *cb=malloc((size_t)n*sizeof(int)); for(int i=0;i<n;i++) cb[i]=newlbl(c);
            emit(c,"  br label %%bb%d\n",cb[0]);
            for(int i=0;i<n;i++){
                lbl(c,cb[i]);
                ValKind vk; const char *vv=gen_rval(c,s->cases[i].val,&vk);
                if(c->rvw) die(0,"case value must be a scalar");
                if(vk==VK_F32) die(0,"case values must be integers");
                const char *cmp=newtmp(c);
                emit(c,"  %s = icmp eq i32 %s, %s\n",cmp,cv,vv);
                emit(c,"  br i1 %s, label %%bb%d, label %%bb%d\n",cmp,cl[i],i+1<n?cb[i+1]:els);
            }
            free(cb);
        }
        /* case bodies are emitted in order; a body that does not terminate
         * branches explicitly into the next case (C fallthrough semantics) */
        c->brk_l[c->nloops]=end; c->nloops++;
        for(int i=0;i<n;i++){
            lbl(c,cl[i]);
            if(div)c->divergent++; gen_block(c,&s->cases[i].body); if(div)c->divergent--;
            if(!c->term){
                int nx = i+1<n ? cl[i+1] : (s->has_default ? dfl : end);
                emit(c,"  br label %%bb%d\n",nx);
            }
        }
        if(s->has_default){
            lbl(c,dfl);
            if(div)c->divergent++; gen_block(c,&s->def_body); if(div)c->divergent--;
            if(!c->term) emit(c,"  br label %%bb%d\n",end);
        }
        c->nloops--;
        lbl(c,end); break; }
    case S_FOR:{
        if(s->for_init) gen_stmt(c,s->for_init);
        int div=s->for_cond&&is_varying(c,s->for_cond);
        if(div) fprintf(stderr,"binc: note (line %d): 'for' bound is data-dependent (varying) — per-thread loop; consider a uniform bound\n",s->for_cond->line);
        int cond=newlbl(c), body=newlbl(c), inc=newlbl(c), en=newlbl(c);
        emit(c,"  br label %%bb%d\n",cond); lbl(c,cond);
        if(s->for_cond){
            const char *cv=gen_cond(c,s->for_cond); emit(c,"  br i1 %s, label %%bb%d, label %%bb%d\n",cv,body,en); }
        else emit(c,"  br label %%bb%d\n",body);
        lbl(c,body);
        c->brk_l[c->nloops]=en; c->cont_l[c->nloops]=inc; c->nloops++;
        if(div)c->divergent++; gen_block(c,&s->then_b); if(div)c->divergent--; c->nloops--;
        if(!c->term) emit(c,"  br label %%bb%d\n",inc);
        lbl(c,inc); if(s->for_incr){ ValKind k; gen_rval(c,s->for_incr,&k); } if(!c->term) emit(c,"  br label %%bb%d\n",cond);
        lbl(c,en); break; }
    }
}
static void gen_block(CG *c, Block *b){ for(size_t i=0;i<b->n;i++) gen_stmt(c,&b->stmts[i]); }

static int explicit_fn(const Function *fn){ for(size_t i=0;i<fn->nparams;i++) if(fn->params[i].ty.kind==T_COORD) return 1; return 0; }
static int meta_count(const Function *fn){
    int n=0, ex=explicit_fn(fn); for(size_t i=0;i<fn->nparams;i++) if(!fn->params[i].ty.array_n) n++;
    if(fn->is_kernel) n += ex?2:1; /* local/group or implicit id */
    return n;
}
static void fn_ptr_str(Function *fn,char *buf,size_t n){
    size_t o=0; int emitted=0, ex=explicit_fn(fn); o+=snprintf(buf+o,n-o,"void (");
    for(size_t i=0;i<fn->nparams;i++){ Param *p=&fn->params[i]; if(p->ty.array_n)continue; if(emitted++)o+=snprintf(buf+o,n-o,", ");
        if(p->ty.kind==T_COORD){ char cl[32]; coord_ll(&p->ty,cl,sizeof cl); o+=snprintf(buf+o,n-o,"%s",cl); }
        else if(p->ty.kind==T_GRID_EXTENT) o+=snprintf(buf+o,n-o,"i32");
        else if(p->ty.kind==T_TEXTURE) o+=snprintf(buf+o,n-o,"%%struct._texture_2d_t addrspace(1)*");
        else if(p->ty.kind==T_SAMPLER) o+=snprintf(buf+o,n-o,"%%struct._sampler_t addrspace(2)*");
        else if(p->ty.is_ptr){ char elt[64]; if(p->ty.matn) mll_of(elt,sizeof elt,p->ty.matn); else type_ll(elt,sizeof elt,p->ty.kind,p->ty.struct_name,p->ty.vecn);
            o+=snprintf(buf+o,n-o,"%s addrspace(%d)*",elt,p->ty.as); }
        else if(fn->is_kernel){ char ll[32]; ll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn); o+=snprintf(buf+o,n-o,"%s addrspace(2)*",ll); }
        else { char ll[32]; ll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn); o+=snprintf(buf+o,n-o,"%s",ll); }
    }
    if(fn->is_kernel){ if(ex){ Param *cp=NULL; for(size_t i=0;i<fn->nparams;i++)if(fn->params[i].ty.kind==T_COORD){cp=&fn->params[i];break;} char cl[32]; coord_ll(&cp->ty,cl,sizeof cl);
            if(emitted++)o+=snprintf(buf+o,n-o,", "); o+=snprintf(buf+o,n-o,"%s, %s",cl,cl);
        } else { if(emitted++)o+=snprintf(buf+o,n-o,", "); o+=snprintf(buf+o,n-o,"i32"); } }
    o+=snprintf(buf+o,n-o,")* @%s",fn->name);
}
static const Program *g_curprog; /* set by emit_air; used by stage string helpers */
static void stage_lit_type(char *buf, size_t n, StructDef *sd);
static void stage_ptr_str(Function *fn,char *buf,size_t n){
    size_t o=0;
    if(fn->ret.kind==T_STRUCT){ StructDef *sd=find_struct(g_curprog,fn->ret.struct_name);
        char lt[256]; stage_lit_type(lt,sizeof lt,sd); o+=snprintf(buf+o,n-o,"%s (",lt); }
    else { char rl[32]; ll_of(rl,sizeof rl,fn->ret.kind,fn->ret.vecn); o+=snprintf(buf+o,n-o,"%s (",rl); }
    int emitted=0;
    for(size_t i=0;i<fn->nparams;i++){ Param *p=&fn->params[i];
        if(p->ty.kind==T_STRUCT && !p->ty.is_ptr && fn->stage==ST_FRAGMENT){
            /* stage-in struct: unpacked as separate args */
            StructDef *sd=find_struct(g_curprog,p->ty.struct_name);
            for(size_t f=0;f<sd->nfields;f++){ char fl[64]; type_ll(fl,sizeof fl,sd->fields[f].ty.kind,sd->fields[f].ty.struct_name,sd->fields[f].ty.vecn);
                if(emitted++)o+=snprintf(buf+o,n-o,", "); o+=snprintf(buf+o,n-o,"%s",fl); }
            continue;
        }
        char pl[64]; type_ll(pl,sizeof pl,p->ty.kind,p->ty.struct_name,p->ty.vecn);
        if(emitted++)o+=snprintf(buf+o,n-o,", ");
        if(p->ty.is_ptr) o+=snprintf(buf+o,n-o,"%s addrspace(%d)*",pl,p->ty.as);
        else o+=snprintf(buf+o,n-o,"%s",pl);
    }
    o+=snprintf(buf+o,n-o,")* @%s",fn->name);
}

/* ---- structured AIR metadata builder ----
 * Node numbers are allocated in one fixed order (all kernels, then all stage
 * functions in source order) and the metadata text is buffered, so the emitted
 * bytes are deterministic no matter how helpers are called. This replaces the
 * previous hand-numbered block with the same output. */
typedef struct { SB sb; int next; } Meta;
static int meta_alloc(Meta *m){ return m->next++; }
static void meta_emit(Meta *m, const char *fmt, ...){
    char b[2048]; va_list ap; va_start(ap,fmt);
    vsnprintf(b,sizeof b,fmt,ap); va_end(ap); sb_put(&m->sb,b);
}
typedef struct { int knode, empty, arglist; int *argnode; int *structnode; } KernelMeta;
typedef struct { Function *fn; int node, empty, outs, leaf, args; int *argnode; int *outnode; int nin, nout; } StageMeta;

/* literal stage-output struct type: <{ <4 x float>, <3 x float> }> */
static void stage_lit_type(char *buf, size_t n, StructDef *sd){
    size_t o=0; o+=snprintf(buf+o,n-o,"<{");
    for(size_t f=0;f<sd->nfields;f++){ char fl[32]; ll_of(fl,sizeof fl,sd->fields[f].ty.kind,sd->fields[f].ty.vecn);
        if(f)o+=snprintf(buf+o,n-o,", "); o+=snprintf(buf+o,n-o,"%s",fl); }
    snprintf(buf+o,n-o,"}>");
}

/* per-kernel !air.kernel metadata (argnode/structnode arrays are builder-allocated) */
static void emit_kernel_meta(Meta *m, const Program *prog, Function *fn, KernelMeta *km,
                             const int *read, const int *written, int nmeta, int np){
    char fptr[2048]; fn_ptr_str(fn,fptr,sizeof fptr);
    meta_emit(m,"!%d = !{}\n",km->empty);
    meta_emit(m,"!%d = !{",km->arglist);
    for(int a=0;a<nmeta;a++){ if(a)meta_emit(m,", "); meta_emit(m,"!%d",km->argnode[a]); }
    meta_emit(m,"}\n");
    meta_emit(m,"!%d = !{%s, !%d, !%d}\n",km->knode,fptr,km->empty,km->arglist);
    int ai=0; Param *cp=NULL; for(size_t x=0;x<fn->nparams;x++)if(fn->params[x].ty.kind==T_COORD){cp=&fn->params[x];break;}
    for(int a=0;a<(int)fn->nparams;a++){ Param *p=&fn->params[a]; if(p->ty.array_n)continue;
        if(p->ty.kind==T_COORD){ char cn[32]; snprintf(cn,sizeof cn,p->ty.coordn==1?"uint":p->ty.coordn==2?"ushort2":"ushort3");
            meta_emit(m,"!%d = !{i32 %d, !\"air.thread_position_in_grid\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",km->argnode[ai++],a,cn,p->name); continue; }
        if(p->ty.kind==T_GRID_EXTENT){ meta_emit(m,"!%d = !{i32 %d, !\"air.threads_per_grid\", !\"air.arg_type_name\", !\"uint\", !\"air.arg_name\", !\"%s\"}\n",km->argnode[ai++],a,p->name); continue; }
        if(p->ty.kind==T_TEXTURE){ const char *elt,*vec,*suf,*an; tex_kinds(p->ty.tex_elt,&elt,&vec,&suf,&an);
            meta_emit(m,"!%d = !{i32 %d, !\"air.texture\", !\"air.location_index\", i32 %d, i32 1, !\"air.read_write\", !\"air.arg_type_name\", !\"texture2d<%s, read_write>\", !\"air.arg_name\", !\"%s\"}\n",km->argnode[ai++],a,a,elt,p->name); continue; }
        if(p->ty.kind==T_SAMPLER){
            meta_emit(m,"!%d = !{i32 %d, !\"air.sampler\", !\"air.location_index\", i32 %d, i32 1, !\"air.arg_type_name\", !\"sampler\", !\"air.arg_name\", !\"%s\"}\n",km->argnode[ai++],a,a,p->name); continue; }
        if(p->ty.is_ptr){ int r=read[a],w=written[a]; const char *acc=(r&&w)?"air.read_write":w?"air.write":"air.read";
            int sz=tsz(p->ty.kind,p->ty.vecn,p->ty.matn),al=tal(p->ty.kind,p->ty.vecn,p->ty.matn); char tnb[64];
            ptn_of(tnb,sizeof tnb,p->ty.kind,p->ty.vecn,p->ty.matn); const char *tn=tnb;
            if(p->ty.kind==T_ATOMIC){ snprintf(tnb,sizeof tnb,"metal::_atomic"); tn=tnb; }
            if(p->ty.kind==T_STRUCT){ StructDef *s=find_struct(prog,p->ty.struct_name);
                if(s){ sz=struct_layout(s,&al); snprintf(tnb,sizeof tnb,"%s",p->ty.struct_name); tn=tnb; }
                else { sz=4; al=4; snprintf(tnb,sizeof tnb,"%s",p->ty.struct_name); tn=tnb; } }
            int an=km->argnode[ai++];
            if(p->ty.kind==T_ATOMIC) meta_emit(m,"!%d = !{i32 %d, !\"air.buffer\", !\"air.location_index\", i32 %d, i32 1, !\"%s\", !\"air.address_space\", i32 %d, !\"air.struct_type_info\", !%d, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",an,a,a,acc,p->ty.as,km->structnode[a],sz,al,tn,p->name);
            else meta_emit(m,"!%d = !{i32 %d, !\"air.buffer\", !\"air.location_index\", i32 %d, i32 1, !\"%s\", !\"air.address_space\", i32 %d, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",an,a,a,acc,p->ty.as,sz,al,tn,p->name); }
        else { char tnb[64]; ptn_of(tnb,sizeof tnb,p->ty.kind,p->ty.vecn,p->ty.matn);
            meta_emit(m,"!%d = !{i32 %d, !\"air.buffer\", !\"air.buffer_size\", i32 %d, !\"air.location_index\", i32 %d, i32 1, !\"air.read\", !\"air.address_space\", i32 2, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",km->argnode[ai++],a,tsz(p->ty.kind,p->ty.vecn,p->ty.matn),a,tsz(p->ty.kind,p->ty.vecn,p->ty.matn),tal(p->ty.kind,p->ty.vecn,p->ty.matn),tnb,p->name); } }
    for(int a=0;a<np;a++) if(km->structnode[a]>=0){ Param *p=&fn->params[a];
        meta_emit(m,"!%d = !{i32 0, i32 4, i32 0, !\"%s\", !\"__s\"}\n",km->structnode[a],type_name(p->ty.atomic_base)); }
    if(cp){ char cn[32]; snprintf(cn,sizeof cn,cp->ty.coordn==1?"uint":cp->ty.coordn==2?"ushort2":"ushort3");
        meta_emit(m,"!%d = !{i32 %d, !\"air.thread_position_in_threadgroup\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s_local\"}\n",km->argnode[ai++],(int)fn->nparams,cn,cp->name);
        meta_emit(m,"!%d = !{i32 %d, !\"air.threadgroup_position_in_grid\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s_group\", !\"air.arg_unused\"}\n",km->argnode[ai++],(int)fn->nparams+1,cn,cp->name);
    } else {
        meta_emit(m,"!%d = !{i32 %d, !\"air.thread_position_in_grid\", !\"air.arg_type_name\", !\"uint\", !\"air.arg_name\", !\"id\"}\n",km->argnode[ai++],(int)fn->nparams);
    }
}

/* per-stage !air.vertex / !air.fragment metadata */
static void emit_stage_meta(Meta *m, const Program *prog, Function *fn, StageMeta *sm){
    char fp[2048]; stage_ptr_str(fn,fp,sizeof fp);
    meta_emit(m,"!%d = !{}\n",sm->empty);
    /* output fields: vertex struct -> position + vertex_output user(locnN);
     * fragment struct -> render_target color(N) + depth; scalar -> single node */
    if(fn->ret.kind==T_STRUCT){
        StructDef *sd=find_struct(prog,fn->ret.struct_name);
        meta_emit(m,"!%d = !{",sm->outs); for(size_t f=0;f<sd->nfields;f++){ if(f)meta_emit(m,", "); meta_emit(m,"!%d",sm->outnode[f]); } meta_emit(m,"}\n");
        int locn=0;
        for(size_t f=0;f<sd->nfields;f++){
            Field *fd=&sd->fields[f]; char tn[64]; ptn_of(tn,sizeof tn,fd->ty.kind,fd->ty.vecn,0);
            if(fn->stage==ST_VERTEX){
                if(fd->attr==1) meta_emit(m,"!%d = !{!\"air.position\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->outnode[f],tn,fd->name);
                else { int ln=fd->attr==5?fd->attr_idx:locn; locn++;
                    meta_emit(m,"!%d = !{!\"air.vertex_output\", !\"user(locn%d)\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->outnode[f],ln,tn,fd->name); }
            } else {
                if(fd->attr==3) meta_emit(m,"!%d = !{!\"air.render_target\", i32 %d, i32 0, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->outnode[f],fd->attr_idx,tn,fd->name);
                else if(fd->attr==4) meta_emit(m,"!%d = !{!\"air.depth\", !\"air.depth_qualifier\", !\"air.any\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->outnode[f],tn,fd->name);
                else die(0,"fragment output fields need [[color(N)]] or [[depth(any)]]");
            }
        }
    } else {
        meta_emit(m,"!%d = !{!%d}\n",sm->outs,sm->leaf);
        if(fn->stage==ST_VERTEX) meta_emit(m,"!%d = !{!\"air.position\", !\"air.arg_type_name\", !\"float4\", !\"air.arg_name\", !\"position\"}\n",sm->leaf);
        else meta_emit(m,"!%d = !{!\"air.render_target\", i32 0, i32 0, !\"air.arg_type_name\", !\"float4\"}\n",sm->leaf);
    }
    meta_emit(m,"!%d = !{",sm->args); for(int a=0;a<sm->nin;a++){ if(a)meta_emit(m,", "); meta_emit(m,"!%d",sm->argnode[a]); } meta_emit(m,"}\n");
    meta_emit(m,"!%d = !{%s, !%d, !%d}\n",sm->node,fp,sm->outs,sm->args);
    int argi=0;
    for(size_t a=0;a<fn->nparams;a++){ Param *p=&fn->params[a]; char tn[64]; ptn_of(tn,sizeof tn,p->ty.kind,p->ty.vecn,p->ty.matn);
        if(fn->stage==ST_FRAGMENT && p->ty.kind==T_STRUCT && !p->ty.is_ptr){
            /* stage-in struct: one metadata node per unpacked field */
            StructDef *sd=find_struct(prog,p->ty.struct_name);
            int locn=0;
            for(size_t f=0;f<sd->nfields;f++){
                Field *fd=&sd->fields[f]; char ftn[64]; ptn_of(ftn,sizeof ftn,fd->ty.kind,fd->ty.vecn,0);
                if(fd->attr==1) meta_emit(m,"!%d = !{i32 %d, !\"air.position\", !\"air.center\", !\"air.no_perspective\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,ftn,fd->name);
                else { int ln=fd->attr==5?fd->attr_idx:locn; locn++;
                    meta_emit(m,"!%d = !{i32 %d, !\"air.fragment_input\", !\"user(locn%d)\", !\"air.center\", !\"%s\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,ln,fd->attr==2?"air.flat":"air.perspective",ftn,fd->name); }
                argi++;
            }
            continue;
        }
        if(fn->stage==ST_VERTEX && p->ty.as==AS_THREAD){ meta_emit(m,"!%d = !{i32 %d, !\"air.vertex_id\", !\"air.arg_type_name\", !\"uint\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,p->name); }
        else if(fn->stage==ST_FRAGMENT && p->ty.kind==T_FLOAT && p->ty.vecn==4){ meta_emit(m,"!%d = !{i32 %d, !\"air.position\", !\"air.center\", !\"air.no_perspective\", !\"air.arg_type_name\", !\"float4\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,p->name); }
        else if(p->ty.is_ptr) meta_emit(m,"!%d = !{i32 %d, !\"air.buffer\", !\"air.location_index\", i32 %d, i32 1, !\"air.read\", !\"air.address_space\", i32 %d, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,argi,p->ty.as,tsz(p->ty.kind,p->ty.vecn,p->ty.matn),tal(p->ty.kind,p->ty.vecn,p->ty.matn),tn,p->name);
        else meta_emit(m,"!%d = !{i32 %d, !\"air.buffer\", !\"air.buffer_size\", i32 %d, !\"air.location_index\", i32 %d, i32 1, !\"air.read\", !\"air.address_space\", i32 2, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,tsz(p->ty.kind,p->ty.vecn,p->ty.matn),argi,tsz(p->ty.kind,p->ty.vecn,p->ty.matn),tal(p->ty.kind,p->ty.vecn,p->ty.matn),tn,p->name);
        argi++;
    }
}

void emit_air(FILE *out, const Program *prog){
    g_curprog=prog;
    memset(builtin_used,0,sizeof builtin_used); memset(atomic_add_used,0,sizeof atomic_add_used);
    fprintf(out,"; generated by binc — works as C, acts as Metal\n");
    fprintf(out,"target datalayout = \"e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32\"\n");
    fprintf(out,"target triple = \"air64_v29-apple-macosx27.0.0\"\n\n");
    /* module-level constant globals: scalar numeric values in address space 2 */
    for(size_t i=0;i<prog->nconsts;i++){ ConstDef *cd=&prog->consts[i];
        char ll[16]; ll_of(ll,sizeof ll,cd->ty.kind,0);
        char val[64];
        if(cd->ty.kind==T_BOOL) snprintf(val,sizeof val,"%s",cd->ival?"true":"false");
        else if(cd->ty.kind==T_FLOAT||cd->ty.kind==T_HALF){
            float fv=(float)(cd->is_int?(double)cd->ival:cd->fval); unsigned bits; memcpy(&bits,&fv,4);
            if(cd->ty.kind==T_HALF) snprintf(val,sizeof val,"fptrunc (float bitcast (i32 %u to float) to half)",bits);
            else snprintf(val,sizeof val,"bitcast (i32 %u to float)",bits);
        }
        else snprintf(val,sizeof val,"%ld",cd->ival);
        fprintf(out,"@_binc_const_%s = internal unnamed_addr addrspace(2) global %s %s, align %d\n",
                cd->name,ll,val,type_align(cd->ty.kind,0));
    }
    if(prog->nconsts) fprintf(out,"\n");
    for(size_t i=0;i<prog->nstructs;i++){ StructDef *s=&prog->structs[i];
        fprintf(out,"%%struct.%s = type { ",s->tag);
        for(size_t j=0;j<s->nfields;j++){ if(j)fprintf(out,", "); char fl[32];
            ll_of(fl,sizeof fl,s->fields[j].ty.kind,s->fields[j].ty.vecn); fprintf(out,"%s",fl); }
        fprintf(out," }\n"); }
    for(size_t fi=0;fi<prog->nfuncs;fi++) for(size_t pi=0;pi<prog->funcs[fi].nparams;pi++){
        Param *p=&prog->funcs[fi].params[pi]; if(!p->ty.array_n)continue;
        char gn[128],elt[64]; snprintf(gn,sizeof gn,"@_binc_smem_%s_%s",prog->funcs[fi].name,p->name); ll_of(elt,sizeof elt,p->ty.kind,p->ty.vecn);
        if(p->ty.array_m) fprintf(out,"%s = internal unnamed_addr addrspace(3) global [%d x [%d x %s]] undef, align %d\n",gn,p->ty.array_n,p->ty.array_m,elt,type_align(p->ty.kind,p->ty.vecn));
        else fprintf(out,"%s = internal unnamed_addr addrspace(3) global [%d x %s] undef, align %d\n",gn,p->ty.array_n,elt,type_align(p->ty.kind,p->ty.vecn));
    }
    int atomic_base=-1;
    for(size_t fi=0;fi<prog->nfuncs;fi++) for(size_t pi=0;pi<prog->funcs[fi].nparams;pi++) if(prog->funcs[fi].params[pi].ty.kind==T_ATOMIC){
        int b=prog->funcs[fi].params[pi].ty.atomic_base; if(atomic_base<0)atomic_base=b; else if(atomic_base!=b) die(0,"atomic buffers in one module must have the same payload type"); }
    if(atomic_base>=0) fprintf(out,"%%\"struct.metal::_atomic\" = type { %s }\n",scalar_ll((TypeKind)atomic_base));
    fprintf(out,"\n");

    typedef struct { int *read,*written; int np, nmeta; } KF; KF *kf=calloc(prog->nfuncs,sizeof(KF));
    for(volatile size_t fi=0;fi<prog->nfuncs;fi++){
        Function *fn=&prog->funcs[fi]; CG c={0};
        SB pr={0},bd={0}; c.pre=&pr; c.body=&bd; c.prog=prog; c.fn=fn; c.tmp=0;
        c.read=calloc(fn->nparams,sizeof(int)); c.written=calloc(fn->nparams,sizeof(int));
        c.scalar_load=calloc(fn->nparams,sizeof(char*)); c.coord_param=-1; c.grid_extent_param=-1;
        for(size_t i=0;i<fn->nparams;i++){
            if(fn->params[i].ty.kind==T_COORD){ c.explicit_domain=1; c.coord_param=(int)i; }
            if(fn->params[i].ty.kind==T_GRID_EXTENT) c.grid_extent_param=(int)i;
        }
        kf[fi].read=c.read; kf[fi].written=c.written; kf[fi].np=(int)fn->nparams;
        kf[fi].nmeta=fn->is_kernel?meta_count(fn):0;
        g_last_line=fn->line; g_last_col=0;
        jmp_buf env; g_recover=&env;
        if(setjmp(env)!=0) continue; /* codegen error: this function is aborted; others still emit */
        /* signature: explicit domains carry coordinate/grid built-ins by value;
         * implicit kernels retain the historical hidden scalar thread id. */
        g_last_line=fn->line; g_last_col=0;
        char sig[2048]; size_t so=0;
        if(fn->is_kernel) so+=snprintf(sig+so,sizeof sig-so,"define void @%s(",fn->name);
        else { char rl[64];
            if(fn->ret.kind==T_VOID) snprintf(rl,sizeof rl,"void");
            else if(fn->ret.kind==T_STRUCT){ StructDef *sd=find_struct(prog,fn->ret.struct_name); stage_lit_type(rl,sizeof rl,sd); }
            else ll_of(rl,sizeof rl,fn->ret.kind,fn->ret.vecn);
            so+=snprintf(sig+so,sizeof sig-so,"define %s%s @%s(",fn->stage==ST_NONE?"internal ":"",rl,fn->name); }
        int emitted=0;
        for(size_t i=0;i<fn->nparams;i++){ Param *p=&fn->params[i];
            if(p->ty.array_n) continue; /* shared arrays are module globals, not ABI args */
            if(fn->stage==ST_FRAGMENT && p->ty.kind==T_STRUCT && !p->ty.is_ptr){
                /* stage-in struct: unpacked as separate arguments */
                StructDef *sd=find_struct(prog,p->ty.struct_name);
                for(size_t f=0;f<sd->nfields;f++){ char fl[64]; type_ll(fl,sizeof fl,sd->fields[f].ty.kind,sd->fields[f].ty.struct_name,sd->fields[f].ty.vecn);
                    if(emitted++)so+=snprintf(sig+so,sizeof sig-so,", ");
                    so+=snprintf(sig+so,sizeof sig-so,"%s noundef %%_%s.%zu",fl,p->name,f); }
                continue;
            }
            if(emitted++)so+=snprintf(sig+so,sizeof sig-so,", ");
            if(p->ty.kind==T_COORD){ char cl[32]; coord_ll(&p->ty,cl,sizeof cl);
                so+=snprintf(sig+so,sizeof sig-so,"%s noundef %%_%s",cl,p->name); }
            else if(p->ty.kind==T_GRID_EXTENT){ so+=snprintf(sig+so,sizeof sig-so,"i32 noundef %%_%s",p->name); }
            else if(p->ty.kind==T_TEXTURE){ so+=snprintf(sig+so,sizeof sig-so,"%%struct._texture_2d_t addrspace(1)* nocapture %%_%s",p->name); }
            else if(p->ty.kind==T_SAMPLER){ so+=snprintf(sig+so,sizeof sig-so,"%%struct._sampler_t addrspace(2)* nocapture %%_%s",p->name); }
            else if(p->ty.is_ptr){ char elt[64]; type_ll(elt,sizeof elt,p->ty.kind,p->ty.struct_name,p->ty.vecn);
                so+=snprintf(sig+so,sizeof sig-so,"%s addrspace(%d)* nocapture noundef %%_%s",elt,p->ty.as,p->name); }
            else if(fn->is_kernel){ char ll[32]; pll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn,p->ty.matn);
                so+=snprintf(sig+so,sizeof sig-so,"%s addrspace(2)* nocapture noundef readonly align %d dereferenceable(%d) %%_%s",ll,tal(p->ty.kind,p->ty.vecn,p->ty.matn),tsz(p->ty.kind,p->ty.vecn,p->ty.matn),p->name); }
            else { char ll[32]; pll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn,p->ty.matn);
                so+=snprintf(sig+so,sizeof sig-so,"%s noundef %%_%s",ll,p->name); } }
        if(fn->is_kernel && c.explicit_domain && c.coord_param>=0){ Param *cp=&fn->params[c.coord_param]; char cl[32]; coord_ll(&cp->ty,cl,sizeof cl);
            so+=snprintf(sig+so,sizeof sig-so,", %s noundef %%_%s_local, %s noundef %%_%s_group",cl,cp->name,cl,cp->name); }
        if(fn->is_kernel && !c.explicit_domain){ if(emitted)so+=snprintf(sig+so,sizeof sig-so,", ");
            so+=snprintf(sig+so,sizeof sig-so,"i32 noundef %%_id"); }
        if(fn->is_kernel) so+=snprintf(sig+so,sizeof sig-so,") local_unnamed_addr");
        else so+=snprintf(sig+so,sizeof sig-so,")");
        if(fn->is_kernel && !c.explicit_domain){ c.idx=malloc(16); snprintf(c.idx,16,"%%t%d",c.tmp++);
            sb_printf(c.pre,"  %s = zext i32 %%_id to i64\n",c.idx); }
        else if(c.explicit_domain && c.coord_param>=0 && fn->params[c.coord_param].ty.coordn==1){
            c.idx=malloc(32); snprintf(c.idx,32,"%%_%s",fn->params[c.coord_param].name);
            const char *z=newtmp(&c); sb_printf(c.pre,"  %s = zext i32 %s to i64\n",z,c.idx); c.idx=(char*)z;
        }
        /* fragment stage-in struct: unpack the argument registers into a struct local */
        if(fn->stage==ST_FRAGMENT) for(size_t pi2=0;pi2<fn->nparams;pi2++){ Param *p=&fn->params[pi2];
            if(p->ty.kind!=T_STRUCT||p->ty.is_ptr) continue;
            StructDef *sd=find_struct(prog,p->ty.struct_name);
            int sal; struct_layout(sd,&sal);
            char *slot=newtmp(&c); sb_printf(c.pre,"  %s = alloca %%struct.%s, align %d\n",slot,p->ty.struct_name,sal);
            for(size_t f=0;f<sd->nfields;f++){
                char pty[96]; pty_str(pty,sizeof pty,sd->fields[f].ty.kind,sd->fields[f].ty.struct_name,0,1,sd->fields[f].ty.vecn,0);
                char ll[32]; ll_of(ll,sizeof ll,sd->fields[f].ty.kind,sd->fields[f].ty.vecn);
                char sp[64]; snprintf(sp,sizeof sp,"%%struct.%s*",p->ty.struct_name);
                const char *fp=newtmp(&c);
                emit(&c,"  %s = getelementptr inbounds %%struct.%s, %s %s, i64 0, i32 %zu\n",fp,p->ty.struct_name,sp,slot,f);
                char an[64]; snprintf(an,sizeof an,"%%_%s.%zu",p->name,f);
                emit(&c,"  store %s %s, %s %s, align %d\n",ll,an,pty,fp,type_align(sd->fields[f].ty.kind,sd->fields[f].ty.vecn));
            }
            c.locs=realloc(c.locs,(c.nlocs+1)*sizeof(Loc));
            c.locs[c.nlocs++]=(Loc){p->name,slot,T_STRUCT,p->ty.struct_name,0,0,0};
        }
        gen_block(&c,&fn->body);
        if(!c.term){ if(fn->ret.kind==T_VOID) emit(&c,"  ret void\n");
            else if(fn->ret.kind==T_STRUCT){ StructDef *sd=find_struct(prog,fn->ret.struct_name);
                char lt[256]; stage_lit_type(lt,sizeof lt,sd); emit(&c,"  ret %s undef\n",lt); }
            else { char rl[32]; ll_of(rl,sizeof rl,fn->ret.kind,fn->ret.vecn);
                emit(&c,"  ret %s %s\n",rl,fn->ret.vecn>1?"zeroinitializer":
                    fn->ret.kind==T_FLOAT||fn->ret.kind==T_HALF?"0.000000e+00":fn->ret.kind==T_BOOL?"false":"0"); } }
        /* a body containing a barrier must be convergent and cannot be nosync (#1); else #0 */
        fprintf(out,"%s #%d {\n",sig,c.uses_sync?1:0);
        fwrite(pr.p,1,pr.n,out); fwrite(bd.p,1,bd.n,out);
        fprintf(out,"}\n\n");
    }
    g_recover=NULL;
    if(atomic_add_used[0]) fprintf(out,"declare float @air.atomic.global.add.f32(float addrspace(1)*, float, i32, i32, i32, i1) local_unnamed_addr\n");
    if(atomic_add_used[1]) fprintf(out,"declare i32 @air.atomic.global.add.i32(i32 addrspace(1)*, i32, i32, i32, i32, i1) local_unnamed_addr\n");
    /* texture intrinsics + the opaque texture/sampler types */
    if(get_samp_used||tex_read_used[0]||tex_read_used[1]||tex_read_used[2]||tex_read_used[3]||
       tex_write_used[0]||tex_write_used[1]||tex_write_used[2]||tex_write_used[3]||
       tex_sample_used[0]||tex_sample_used[1]||tex_sample_used[2]||tex_sample_used[3]){
        fprintf(out,"%%struct._texture_2d_t = type opaque\n%%struct._sampler_t = type opaque\n");
        if(get_samp_used) fprintf(out,"declare %%struct._sampler_t addrspace(2)* @air.get_read_sampler() local_unnamed_addr\n");
        static const char *sufs[4]={"v4f32","v4f16","v4i32","v4u32"};
        static const char *vecs[4]={"<4 x float>","<4 x half>","<4 x i32>","<4 x i32>"};
        for(int i=0;i<4;i++){
            if(tex_read_used[i]) fprintf(out,"declare { %s, i8 } @air.read_texture_2d.%s(%%struct._texture_2d_t addrspace(1)* nocapture readonly, %%struct._sampler_t addrspace(2)*, <2 x i32>, <2 x i32>, i32, i32) local_unnamed_addr\n",vecs[i],sufs[i]);
            if(tex_write_used[i]) fprintf(out,"declare void @air.write_texture_2d.%s(%%struct._texture_2d_t addrspace(1)* nocapture, <2 x i32>, %s, i32, i32) local_unnamed_addr\n",sufs[i],vecs[i]);
            if(tex_sample_used[i]) fprintf(out,"declare { %s, i8 } @air.sample_texture_2d.%s(%%struct._texture_2d_t addrspace(1)* nocapture readonly, %%struct._sampler_t addrspace(2)* nocapture readonly, <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr\n",vecs[i],sufs[i]);
        }
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
    /* structured AIR metadata — node numbers allocated in fixed order:
     * all kernels first, then all stage functions, in source order. */
    size_t nk=0; for(size_t fi=0;fi<prog->nfuncs;fi++) if(prog->funcs[fi].is_kernel) nk++;
    size_t ns=0; for(size_t fi=0;fi<prog->nfuncs;fi++) if(prog->funcs[fi].stage!=ST_NONE) ns++;
    Meta meta={0}; meta.next=13;
    KernelMeta *km=calloc(nk?nk:1,sizeof(KernelMeta));
    StageMeta *sm=calloc(ns?ns:1,sizeof(StageMeta));
    size_t ki=0;
    for(size_t fi=0;fi<prog->nfuncs;fi++){ if(!prog->funcs[fi].is_kernel) continue;
        km[ki].knode=meta_alloc(&meta); km[ki].empty=meta_alloc(&meta); km[ki].arglist=meta_alloc(&meta);
        int na=kf[fi].nmeta; km[ki].argnode=malloc((na?na:1)*sizeof(int)); for(int a=0;a<na;a++)km[ki].argnode[a]=meta_alloc(&meta);
        km[ki].structnode=malloc((kf[fi].np?kf[fi].np:1)*sizeof(int)); for(int a=0;a<kf[fi].np;a++) km[ki].structnode[a]=-1;
        for(int a=0;a<kf[fi].np;a++) if(prog->funcs[fi].params[a].ty.kind==T_ATOMIC) km[ki].structnode[a]=meta_alloc(&meta);
        ki++; }
    size_t si=0;
    for(size_t fi=0;fi<prog->nfuncs;fi++) if(prog->funcs[fi].stage!=ST_NONE){
        Function *sf=&prog->funcs[fi];
        size_t nin=0;
        for(size_t a=0;a<sf->nparams;a++){
            if(sf->stage==ST_FRAGMENT && sf->params[a].ty.kind==T_STRUCT && !sf->params[a].ty.is_ptr){
                StructDef *sd=find_struct(prog,sf->params[a].ty.struct_name);
                nin += sd?sd->nfields:1;
            } else nin++;
        }
        size_t nout = sf->ret.kind==T_STRUCT ? (find_struct(prog,sf->ret.struct_name)?find_struct(prog,sf->ret.struct_name)->nfields:1) : 1;
        sm[si].fn=sf;
        sm[si].node=meta_alloc(&meta); sm[si].empty=meta_alloc(&meta); sm[si].outs=meta_alloc(&meta);
        sm[si].leaf=meta_alloc(&meta); sm[si].args=meta_alloc(&meta);
        sm[si].nin=(int)nin; sm[si].nout=(int)nout;
        sm[si].argnode=malloc((nin?nin:1)*sizeof(int));
        for(size_t a=0;a<nin;a++)sm[si].argnode[a]=meta_alloc(&meta);
        sm[si].outnode=malloc((nout?nout:1)*sizeof(int));
        for(size_t o=0;o<nout;o++)sm[si].outnode[o]=meta_alloc(&meta);
        si++; }
    meta_emit(&meta,"!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6}\n");
    meta_emit(&meta,"!air.kernel = !{"); for(size_t k=0;k<nk;k++){ if(k)meta_emit(&meta,", "); meta_emit(&meta,"!%d",km[k].knode); } meta_emit(&meta,"}\n");
    meta_emit(&meta,"!air.vertex = !{"); int first=1; for(size_t s=0;s<ns;s++){ if(sm[s].fn->stage!=ST_VERTEX) continue; if(!first)meta_emit(&meta,", "); first=0; meta_emit(&meta,"!%d",sm[s].node); } meta_emit(&meta,"}\n");
    meta_emit(&meta,"!air.fragment = !{"); first=1; for(size_t s=0;s<ns;s++){ if(sm[s].fn->stage!=ST_FRAGMENT) continue; if(!first)meta_emit(&meta,", "); first=0; meta_emit(&meta,"!%d",sm[s].node); } meta_emit(&meta,"}\n");
    meta_emit(&meta,"!air.compile_options = !{!7, !8}\n!llvm.ident = !{!9}\n!air.version = !{!10}\n!air.language_version = !{!11}\n!air.source_file_name = !{!12}\n\n");
    meta_emit(&meta,"!0 = !{i32 2, !\"SDK Version\", [2 x i32] [i32 27, i32 0]}\n!1 = !{i32 1, !\"wchar_size\", i32 4}\n!2 = !{i32 7, !\"air.max_device_buffers\", i32 31}\n!3 = !{i32 7, !\"air.max_constant_buffers\", i32 31}\n!4 = !{i32 7, !\"air.max_threadgroup_buffers\", i32 31}\n!5 = !{i32 7, !\"air.max_textures\", i32 128}\n!6 = !{i32 7, !\"air.max_samplers\", i32 16}\n!7 = !{!\"air.compile.denorms_disable\"}\n!8 = !{!\"air.compile.fast_math_enable\"}\n!9 = !{!\"BinC compiler v0.0.1 (bootstrap, in C)\"}\n!10 = !{i32 2, i32 9, i32 0}\n!11 = !{!\"Metal\", i32 4, i32 1, i32 0}\n!12 = !{!\"binc\"}\n");
    ki=0;
    for(size_t fi=0;fi<prog->nfuncs;fi++){ if(!prog->funcs[fi].is_kernel) continue;
        emit_kernel_meta(&meta,prog,&prog->funcs[fi],&km[ki],kf[fi].read,kf[fi].written,kf[fi].nmeta,kf[fi].np);
        ki++; }
    /* stage emission preserves the previous grouping: vertices first, then fragments */
    for(size_t s=0;s<ns;s++) if(sm[s].fn->stage==ST_VERTEX) emit_stage_meta(&meta,prog,sm[s].fn,&sm[s]);
    for(size_t s=0;s<ns;s++) if(sm[s].fn->stage==ST_FRAGMENT) emit_stage_meta(&meta,prog,sm[s].fn,&sm[s]);
    fwrite(meta.sb.p,1,meta.sb.n,out);
}
