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
        case T_INT32: case T_UINT32: return "i32"; case T_VOID: return "void"; default: return "float"; } }
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
static void mll_of(char *buf,size_t n,int matn,int matm){
    if(matm&&matm!=matn) die(0,"non-square matrix (%dx%d) not supported in codegen yet (Phase 6+)",matn,matm);
    snprintf(buf,n,"[%d x <%d x float>]",matn,matn); }
static void ll_of(char *buf,size_t n,TypeKind k,int vecn);
/* matrix-aware value type: matrix -> [N x <N x float>], else scalar/vector */
static void pll_of(char *buf,size_t n,TypeKind k,int vecn,int matn,int matm){
    if(matn) mll_of(buf,n,matn,matm); else ll_of(buf,n,k,vecn); }
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
static const Program *g_curprog; /* set by emit_air; used by stage helpers and struct layout */
static int struct_layout_r(const Program *prog, StructDef *s, int *al){
    int off=0,m=1;
    for(size_t i=0;i<s->nfields;i++){ int fa,fs;
        if(s->fields[i].ty.kind==T_STRUCT&&s->fields[i].ty.struct_name){
            StructDef *sub=prog?find_struct(prog,s->fields[i].ty.struct_name):NULL;
            if(sub){ int sa; fs=struct_layout_r(prog,sub,&sa); fa=sa; }
            else { fa=1; fs=1; }
        } else { fa=tal(s->fields[i].ty.kind,s->fields[i].ty.vecn,s->fields[i].ty.matn);
            fs=tsz(s->fields[i].ty.kind,s->fields[i].ty.vecn,s->fields[i].ty.matn); }
        if(s->fields[i].ty.array_n) fs *= s->fields[i].ty.array_n*(s->fields[i].ty.array_m?s->fields[i].ty.array_m:1);
        if(fa>m)m=fa; off=(off+fa-1)&~(fa-1); off+=fs; }
    *al=m; return (off+m-1)&~(m-1);
}
static int struct_layout(StructDef *s, int *al){ return struct_layout_r(g_curprog,s,al); }

typedef struct { char *name; char *slot; TypeKind kind; char *sname; int vecn; int matn; int is_const; int an, am; } Loc;
typedef struct {
    SB *pre,*body; const Program *prog; Function *fn; size_t fidx; /* fidx: fn's slot in prog->funcs (re-fetch fn after realloc) */
    int tmp; char *idx; int explicit_domain; int coord_param; int grid_extent_param;
    int uses_local_coord, uses_group_coord; int *read,*written; char **scalar_load;
    Loc *locs; size_t nlocs;
    int term; /* current block terminated */
    int blk_empty; /* current block has no instructions yet (AIR rejects empty blocks) */
    int lblc; /* label counter (separate from tmp) */
    int curbb; /* last emitted label number (phi predecessors) */
    int brk_l[32], cont_l[32], nloops; /* break/continue label stack */
    int uses_sync; /* body contains a barrier -> function must be convergent, not nosync */
    int divergent; /* current control-flow region is varying/divergent */
    int rvw; /* vector width of the last gen_rval result, 0 = scalar */
    int rmat; /* matrix width of the last gen_rval result, 0 = not a matrix */
    const char *rstruct; /* struct tag of the last gen_rval result, NULL = not a struct value */
} CG;
static char *newtmp(CG *c){ char *s=malloc(16); snprintf(s,16,"%%t%d",c->tmp++); return s; }
static int newlbl(CG *c){ return c->lblc++; }
static void emit(CG *c,const char *fmt,...){ va_list a; va_start(a,fmt); char b[1024]; vsnprintf(b,sizeof b,fmt,a); va_end(a); sb_put(c->body,b); c->blk_empty=0;
 }
static void lbl(CG *c,int n){
    if(c->blk_empty) emit(c,"  br label %%bb%d\n",n); /* AIR rejects empty blocks: jump from the empty predecessor */
    char b[32]; snprintf(b,sizeof b,"bb%d:\n",n); sb_put(c->body,b); c->term=0; c->blk_empty=1; c->curbb=n;
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
    if(from==VK_I32&&to==VK_I1){ const char *r=newtmp(c); emit(c,"  %s = icmp ne i32 %s, 0\n",r,v); return r; } /* int -> bool (CondMask(Flags & 0x100, ...)) */
    if(from==VK_U32&&to==VK_I1){ const char *r=newtmp(c); emit(c,"  %s = icmp ne i32 %s, 0\n",r,v); return r; }
    return v;
}

/* resolve a name to: local | pointer param idx | scalar param idx | module constant | texture/sampler */
typedef enum { R_NONE,R_LOCAL,R_PTR,R_SCALAR,R_COORD,R_EXTENT,R_CONST,R_TEXTURE,R_SAMPLER } RKind;
static RKind resolve(CG *c, const char *name, int *out){
    for(size_t i=0;i<c->nlocs;i++) if(!strcmp(c->locs[i].name,name)){ *out=(int)i; if(getenv("BINC_DEBUG_IW")&&!strcmp(name,"Primitive")) fprintf(stderr,"DBG resv: Primitive -> R_LOCAL %d nlocs=%zu\n",(int)i,c->nlocs); return R_LOCAL; }
    for(size_t i=0;i<c->fn->nparams;i++) if(!strcmp(c->fn->params[i].name,name)){
        *out=(int)i;
        if(getenv("BINC_DEBUG_IW")&&!strcmp(name,"Primitive")) fprintf(stderr,"DBG resp: %s -> param %zu fn=%s np=%zu\n",name,i,c->fn->name,c->fn->nparams);
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
typedef struct { TypeKind tk; char *sname; int as; int pi; int is_local; int vecn; int matn; int an, am; } LInfo;
static void shared_name(CG *c, int pi, char *buf, size_t n){
    snprintf(buf,n,"@_binc_smem_%s_%s",c->fn->name,c->fn->params[pi].name);
}
static void pty_str(char *buf,size_t n,TypeKind tk,char *sname,int as,int is_local,int vecn,int matn){
    char elt[64];
    if(matn) mll_of(elt,sizeof elt,matn,0); else type_ll(elt,sizeof elt,tk,sname,vecn);
    if(is_local) snprintf(buf,n,"%s*",elt); else snprintf(buf,n,"%s addrspace(%d)*",elt,as); }

/* address of element p[ix] (i64 index value) for pointer param pi */
static char *element_ptr_idx(CG *c,int pi,const char *ix){
    Param *pr=&c->fn->params[pi]; char elt[64];
    if(pr->ty.matn) mll_of(elt,sizeof elt,pr->ty.matn,pr->ty.matm); else type_ll(elt,sizeof elt,pr->ty.kind,pr->ty.struct_name,pr->ty.vecn);
    char *s=newtmp(c);
    emit(c,"  %s = getelementptr inbounds %s, %s addrspace(%d)* %%_%s, i64 %s\n",s,elt,elt,pr->ty.as,pr->name,ix);
    return s;
}
static char *gen_lval(CG *c, Expr *e, LInfo *li, int mark);
static const char *gen_cond(CG *c, Expr *e);
static const char *gen_rval(CG *c, Expr *e, ValKind *k);
static Expr *rw_copy(Expr *e){ if(!e) return NULL; Expr *n=calloc(1,sizeof(Expr)); *n=*e;
    n->name=e->name?strdup(e->name):NULL; n->field=e->field?strdup(e->field):NULL;
    n->operand=rw_copy(e->operand); n->lhs=rw_copy(e->lhs); n->rhs=rw_copy(e->rhs);
    n->callee=rw_copy(e->callee);
    if(e->args&&e->nargs){ n->args=calloc(e->nargs,sizeof(Expr*)); for(size_t i=0;i<e->nargs;i++) n->args[i]=rw_copy(e->args[i]); }
    return n; }

/* ---- template (monomorphized) functions ----
 * A template<T> function is skipped at emission; each call site with a concrete
 * T instantiates a copy whose name is "<fn>.<typekey>", appended to prog->funcs
 * (the emit loop re-reads nfuncs, so instantiations made while codegen'ing a
 * body are picked up). The instantiated function shares the template's body AST;
 * type variables inside it (S_DECL / E_CAST) resolve via fn->tvar_ty. */
static Type bind_ty(const Function *fn, Type ty){
    if(ty.tvar){ if(!fn->tvar) die(0,"unbound type variable %s",ty.tvar); return fn->tvar_ty; }
    return ty;
}
static void type_key(const Type *ty, char *buf, size_t n){
    if(ty->kind==T_STRUCT) snprintf(buf,n,"s%s",ty->struct_name);
    else if(ty->kind==T_TVAR) snprintf(buf,n,"tvar");
    else if(ty->matn) snprintf(buf,n,"m%d",ty->matn);
    else if(ty->vecn>1) snprintf(buf,n,"v%d%s",ty->vecn,ty->kind==T_HALF?"f16":"f32");
    else snprintf(buf,n,"%s",ty->kind==T_INT32?"i32":ty->kind==T_UINT32?"u32":
        ty->kind==T_FLOAT?"f32":ty->kind==T_HALF?"f16":ty->kind==T_BOOL?"b1":"?");
}
typedef struct { char *key; size_t fidx; } InstRec;
static InstRec *insts=NULL; static size_t ninsts=0;
static size_t inst_lookup(const char *key){ for(size_t i=0;i<ninsts;i++) if(!strcmp(insts[i].key,key)) return insts[i].fidx; return (size_t)-1; }

/* static type of an expression, for template argument inference */
static int vec_name(const char *s, TypeKind *k, int *n); /* forward (defined below) */
typedef struct { const char *name,*ll; int nargs; TypeKind ret, a0, a1; int vec_ok; } Builtin;
/* builtins: AIR spellings probed from the metal frontend (metal -emit-llvm -S on MSL using
 * metal::sqrt etc.; the module sets air.compile.fast_math_enable, hence the fast_ variants).
 * sync() is the threadgroup barrier: air.wg.barrier(i32 2, i32 5, i32 1). */
static Builtin builtins[]={
    {"sqrt","air.fast_sqrt.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"fabs","air.fast_fabs.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"floor","air.fast_floor.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"ceil","air.fast_ceil.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"round","air.fast_round.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"mad","air.fma.f32",3,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"sin","air.fast_sin.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"cos","air.fast_cos.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"exp","air.fast_exp.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"log","air.fast_log.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"exp2","air.fast_exp2.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"log2","air.fast_log2.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"fmin","air.fast_fmin.f32",2,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"fmax","air.fast_fmax.f32",2,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"pow","air.fast_pow.f32",2,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"atan2","air.fast_atan2.f32",2,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"rsqrt","air.fast_rsqrt.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"sign","air.sign.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"imin","air.min.s.i32",2,T_INT32,T_INT32,T_INT32,1},
    {"imax","air.max.s.i32",2,T_INT32,T_INT32,T_INT32,1},
    /* HLSL aliases: min/max are type-generic in HLSL; the float forms are
     * exact for the differential range (int operands convert to float) */
    {"min","air.fast_fmin.f32",2,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"max","air.fast_fmax.f32",2,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"abs","air.fast_fabs.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"saturate","air.fast_saturate.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"tan","air.fast_tan.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"asin","air.fast_asin.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"acos","air.fast_acos.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"atan","air.fast_atan.f32",1,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"fmod","air.fast_fmod.f32",2,T_FLOAT,T_FLOAT,T_FLOAT,1},
    {"sync","air.wg.barrier",0,T_VOID,T_VOID,T_VOID,0},
};
static void expr_type_of(CG *c, Expr *e, Type *out){
    memset(out,0,sizeof *out);
    switch(e->kind){
    case E_ICONST: out->kind=T_INT32; break;
    case E_FCONST: out->kind=T_FLOAT; break;
    case E_BOOL: out->kind=T_BOOL; break;
    case E_IDENT:{ int idx; RKind r=resolve(c,e->name,&idx);
        if(getenv("BINC_DEBUG_IW")&&!strcmp(e->name,"__uniforms")) fprintf(stderr,"DBG etypu: r=%d idx=%d fn=%s nparams=%zu\n",r,idx,c->fn->name,c->fn->nparams);
        if(r==R_LOCAL){ out->kind=c->locs[idx].kind; out->struct_name=c->locs[idx].sname;
            out->vecn=c->locs[idx].vecn; out->matn=c->locs[idx].matn; }
        else if(r==R_CONST){ out->kind=c->prog->consts[idx].ty.kind; }
        else if(r==R_PTR||r==R_SCALAR){ Type t=c->fn->params[idx].ty; t.tvar=NULL; *out=t; }
        else if(r==R_COORD||r==R_EXTENT){ Type t=c->fn->params[idx].ty; t.tvar=NULL; *out=t; }
        else { out->kind=T_INT32; }
        break; }
    case E_CAST: { Type t=e->cty; t.tvar=NULL; *out=t; break; }
    case E_FIELD:{ Type base; expr_type_of(c,e->operand,&base);
        if(base.kind==T_STRUCT){ for(size_t i=0;i<c->prog->nstructs;i++)
            if(!strcmp(c->prog->structs[i].tag,base.struct_name)){
                StructDef *sd=&c->prog->structs[i];
                for(size_t j=0;j<sd->nfields;j++) if(!strcmp(sd->fields[j].name,e->field)){ Type t=sd->fields[j].ty; t.tvar=NULL; *out=t; return; }
                break; } }
        else if(base.kind==T_COORD||base.kind==T_GRID_EXTENT){ out->kind=T_UINT32; out->vecn=0; } /* tid.x -> uint */
        else { *out=base; size_t fl=strlen(e->field); if(fl==1) out->vecn=0; else if(fl<=4) out->vecn=(int)fl; } /* vector swizzle: the swizzle width (v.xy -> 2) */
        break; }
    case E_CALL:{
        /* user function: best-overload by arg compatibility (mirrors the codegen
         * ranker) — a plain first-name match is wrong for overloaded helpers
         * like MakePrecise(float / float2 / float3 / float4) */
        { Type best={0}; int found=0, bestrk=1<<30;
        for(size_t i=0;i<c->prog->nfuncs;i++){
            Function *cf=&c->prog->funcs[i];
            if(strcmp(cf->name,e->name)) continue;
            if(e->nargs>cf->nparams) continue;
            if(e->nargs<cf->nparams){ int nd=0;
                for(size_t q=e->nargs;q<cf->nparams;q++) if(!cf->params[q].def){ nd=1; break; }
                if(nd) continue; }
            int rk=0, ok=1;
            for(size_t ai=0;ai<e->nargs;ai++){
                Type at={0}; expr_type_of(c,e->args[ai],&at);
                Type pt=cf->params[ai].ty;
                if(pt.tvar){ ok=0; break; }
                if(at.kind!=pt.kind){
                    if(at.kind==T_INT32&&(pt.kind==T_FLOAT||pt.kind==T_HALF)&&!at.vecn&&!pt.vecn){ rk+=1; continue; }
                    if(at.kind==T_INT32&&pt.kind==T_FLOAT&&!at.vecn&&pt.vecn>0){ rk+=3; continue; } /* int scalar splat */
                    ok=0; break; }
                if(at.vecn!=pt.vecn){
                    if(at.vecn==0&&pt.vecn>0){ rk+=2; continue; }
                    if(at.vecn>pt.vecn&&pt.vecn>0){ rk+=2; continue; }
                    ok=0; break; }
            }
            if(!ok) continue;
            if(rk<bestrk){ bestrk=rk; best=cf->ret; found=1; }
        }
        if(found){ best.tvar=NULL; *out=best; break; } }
        /* builtin calls (mad, sqrt, ...): ret kind + arg-derived width so the
         * overload ranker sees float3 mad() results, not T_INT32 */
        for(size_t bi2=0;bi2<sizeof builtins/sizeof *builtins;bi2++)
            if(!strcmp(builtins[bi2].name,e->name)){
                out->kind=builtins[bi2].ret;
                if(builtins[bi2].vec_ok&&e->nargs){
                    for(size_t ai2=0;ai2<e->nargs;ai2++){
                        Type at2={0}; expr_type_of(c,e->args[ai2],&at2);
                        if(at2.vecn>out->vecn) out->vecn=at2.vecn;
                    }
                }
                break;
            }
        if(out->kind) break; /* matched a builtin */
        /* math spell functions (dot/cross/length/normalize/...) — codegen has
         * a dedicated spell table; type them so overload rankers see the
         * right width instead of T_INT32 */
        { static const char *scalars[]={"dot","length","distance",NULL};
          static const char *width_of_arg[]={"normalize","reflect","clamp","lerp","mix","step","smoothstep","fract","frac","mod","radians","degrees","sign","cross",NULL};
          int matched=0;
          for(int si=0;scalars[si]&&!matched;si++) if(!strcmp(scalars[si],e->name)){ out->kind=T_FLOAT; out->vecn=0; matched=1; }
          for(int si=0;width_of_arg[si]&&!matched;si++) if(!strcmp(width_of_arg[si],e->name)){
              if(e->nargs>=1){ expr_type_of(c,e->args[0],out); }
              else { out->kind=T_FLOAT; }
              matched=1; }
          if(matched) break; }
        /* vector/scalar constructors: float2..uint4, float/int/uint/bool */
        { TypeKind ck; int cn; if(vec_name(e->name,&ck,&cn)){ out->kind=ck; out->vecn=cn; break; } }
        if(!strcmp(e->name,"float")||!strcmp(e->name,"half")){ out->kind=T_FLOAT; break; }
        if(!strcmp(e->name,"int")){ out->kind=T_INT32; break; }
        if(!strcmp(e->name,"uint")){ out->kind=T_UINT32; break; }
        if(!strcmp(e->name,"bool")){ out->kind=T_BOOL; break; }
        out->kind=T_INT32; break; }
    case E_INDEX:{ Type base; expr_type_of(c,e->operand,&base); base.vecn=0; base.matn=0; base.array_n=0; base.array_m=0; *out=base; break; }
    case E_DEREF:{ Type base; expr_type_of(c,e->operand,&base); base.is_ptr=0; *out=base; break; }
    case E_ASSIGN: expr_type_of(c,e->rhs,out); break;
    case E_TERNARY: expr_type_of(c,e->rhs,out); break;
    case E_INCDEC: case E_NEG: case E_NOT: case E_COMPL: expr_type_of(c,e->operand,out); break;
    case E_BIN:{ Type a,b; expr_type_of(c,e->lhs,&a); expr_type_of(c,e->rhs,&b);
        if(a.kind==T_FLOAT||b.kind==T_FLOAT) out->kind=T_FLOAT;
        else if(a.kind==T_HALF||b.kind==T_HALF) out->kind=T_HALF;
        else if(a.kind==T_UINT32||b.kind==T_UINT32) out->kind=T_UINT32;
        else if(a.kind==T_COORD||b.kind==T_COORD) out->kind=T_UINT32;
        else out->kind=T_INT32;
        out->vecn = a.vecn> b.vecn ? a.vecn : b.vecn; break; }
    case E_CMP: case E_LOG: out->kind=T_BOOL; { Type ot={0}; expr_type_of(c,e->lhs,&ot); out->vecn=ot.vecn; } break; /* vector compare keeps the width (N.xy >= 0 -> bool2) */
    default: out->kind=T_INT32; break;
    }
}
static Type expr_type(CG *c, Expr *e){ Type t={0}; expr_type_of(c,e,&t); return t; }

static size_t instantiate(Program *prog, Function *tpl, const Type *concrete);
static size_t instantiate(Program *prog, Function *tpl, const Type *concrete){
    char key[128]; type_key(concrete,key,sizeof key);
    Function inst=*tpl;
    inst.name=malloc(strlen(tpl->name)+strlen(key)+2);
    sprintf(inst.name,"%s.%s",tpl->name,key);
    inst.is_template=0;
    inst.tvar=tpl->tvar?strdup(tpl->tvar):NULL;
    inst.tvar_ty=*concrete; inst.tvar_ty.tvar=NULL;
    inst.params=calloc(tpl->nparams,sizeof(Param));
    for(size_t i=0;i<tpl->nparams;i++){
        inst.params[i].name=strdup(tpl->params[i].name);
        inst.params[i].un=tpl->params[i].un;
        inst.params[i].ty=tpl->params[i].ty;
        if(inst.params[i].ty.tvar) inst.params[i].ty=*concrete;
        inst.params[i].ty.tvar=NULL;
    }
    inst.ret=tpl->ret;
    if(inst.ret.tvar) inst.ret=*concrete;
    inst.ret.tvar=NULL;
    prog->funcs=realloc(prog->funcs,(prog->nfuncs+1)*sizeof(Function));
    size_t fi=prog->nfuncs; prog->funcs[fi]=inst; prog->nfuncs++;
    insts=realloc(insts,(ninsts+1)*sizeof(InstRec));
    insts[ninsts].key=strdup(inst.name); insts[ninsts].fidx=fi; ninsts++;
    return fi;
}

/* matrix width of a name/field expression, 0 if not a matrix */
static int matrix_n(CG *c, Expr *e){
    if(e->kind==E_IDENT){
        int i; RKind r=resolve(c,e->name,&i);
        if(r==R_LOCAL) return c->locs[i].matn;
        if(r==R_PTR) return c->fn->params[i].ty.matn;
        if(r==R_SCALAR) return c->fn->params[i].ty.matn; /* matrix parameter (float4x4 M) */
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
    char mty[32]; mll_of(mty,sizeof mty,mn,0);
    const char *r=newtmp(c);
    emit(c,"  %s = extractvalue %s %s, %d\n",r,mty,m,col);
    return r;
}
/* insert column `col` into a matrix aggregate */
static const char *mat_setcol(CG *c, const char *m, int mn, int col, const char *v){
    char mty[32]; mll_of(mty,sizeof mty,mn,0);
    const char *r=newtmp(c);
    emit(c,"  %s = insertvalue %s %s, <%d x float> %s, %d\n",r,mty,m,mn,v,col);
    return r;
}
/* transpose a matrix register: t[c][r] = m[r][c] */
static const char *mat_transpose(CG *c, const char *m, int mn, ValKind *k);
/* element of a matrix register at column col, row r */
static const char *mat_elem(CG *c, const char *m, int mn, int col, int r){    const char *colv=mat_col(c,m,mn,col);
    const char *x=newtmp(c);
    emit(c,"  %s = extractelement <%d x float> %s, i32 %d\n",x,mn,colv,r);
    return x;
}
/* transpose a matrix register: t[c][r] = m[r][c] */
static const char *mat_transpose(CG *c, const char *m, int mn, ValKind *k){
    const char *res="undef";
    for(int cc=0;cc<mn;cc++){
        const char *colv="undef";
        for(int rr=0;rr<mn;rr++){
            const char *me=mat_elem(c,m,mn,rr,cc); /* m[rr][cc] -> t[cc][rr] */
            const char *ins=newtmp(c);
            emit(c,"  %s = insertelement <%d x float> %s, float %s, i32 %d\n",ins,mn,colv,me,rr);
            colv=ins;
        }
        res=mat_setcol(c,res,mn,cc,colv);
    }
    *k=VK_F32; c->rvw=0; c->rmat=mn; return res;
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

/* promote a half-typed value to f32 (vector-aware); compute is done in f32 */
static const char *swizzle_read(CG *c, const char *vec, const char *vty, const char *ety, const int *idxs, int nc);
static const char *h2f(CG *c, const char *v, int vecn){
    const char *w=newtmp(c);
    if(vecn>1) emit(c,"  %s = fpext <%d x half> %s to <%d x float>\n",w,vecn,v,vecn);
    else emit(c,"  %s = fpext half %s to float\n",w,v);
    return w;
}
/* demote an f32 value for half storage (vector-aware) */
static const char *f2h(CG *c, const char *v, int vecn){
    const char *w=newtmp(c);
    if(vecn>1) emit(c,"  %s = fptrunc <%d x float> %s to <%d x half>\n",w,vecn,v,vecn);
    else emit(c,"  %s = fptrunc float %s to half\n",w,v);
    return w;
}

/* load an lvalue, promoting half to float; *k gets the expression-level kind, c->rvw the vector width */
static const char *emit_load_t(CG *c, LInfo *li, const char *addr, ValKind *k){
    if(li->tk==T_STRUCT){
        StructDef *sd=find_struct(c->prog,li->sname);
        if(!sd) die(0,"unknown struct %s",li->sname);
        int sal; struct_layout(sd,&sal);
        char sn[96]; snprintf(sn,sizeof sn,"%%struct.%s",li->sname);
        char sp[96];
        if(li->as) snprintf(sp,sizeof sp,"%%struct.%s addrspace(%d)*",li->sname,li->as);
        else snprintf(sp,sizeof sp,"%%struct.%s*",li->sname);
        const char *v=newtmp(c);
        emit(c,"  %s = load %s, %s %s, align %d\n",v,sn,sp,addr,sal);
        *k=VK_I32; c->rvw=0; c->rstruct=li->sname; return v;
    }
    if(li->matn){ /* matrix load: [N x <N x float>] */
        char mty[32]; mll_of(mty,sizeof mty,li->matn,0);
        char pty[96]; pty_str(pty,sizeof pty,li->tk,li->sname,li->as,li->is_local,li->vecn,li->matn);
        const char *v=newtmp(c);
        emit(c,"  %s = load %s, %s %s, align %d\n",v,mty,pty,addr,mat_align(li->matn));
        *k=VK_F32; c->rvw=0; c->rmat=li->matn; return v;
    }
    if(li->an>0){ /* packed vector / array field: [N x T] or [N x [M x T]] */
        char lty[64], pty[96];
        if(li->am>0) snprintf(lty,sizeof lty,"[%d x [%d x %s]]",li->an,li->am,scalar_ll(li->tk));
        else snprintf(lty,sizeof lty,"[%d x %s]",li->an,scalar_ll(li->tk));
        if(li->as) snprintf(pty,sizeof pty,"%s addrspace(%d)*",lty,li->as);
        else snprintf(pty,sizeof pty,"%s*",lty);
        const char *v=newtmp(c);
        emit(c,"  %s = load %s, %s %s, align 4\n",v,lty,pty,addr);
        *k=scalar_vk(li->tk);
        if(!li->am&&li->an<=4){ /* packed vector: [3 x float] -> <3 x float> */
            char vty[32]; snprintf(vty,sizeof vty,"<%d x %s>",li->an,scalar_ll(li->tk));
            const char *acc="undef";
            for(int i=0;i<li->an;i++){
                const char *el=newtmp(c);
                emit(c,"  %s = extractvalue %s %s, %d\n",el,lty,v,i);
                const char *ins=newtmp(c);
                emit(c,"  %s = insertelement %s %s, %s %s, i32 %d\n",ins,vty,acc,scalar_ll(li->tk),el,i);
                acc=ins;
            }
            c->rvw=li->an; return acc;
        }
        c->rvw=0; return v;
    }
    char pty[96]; pty_str(pty,sizeof pty,li->tk,li->sname,li->as,li->is_local,li->vecn,li->matn);
    char ll[32]; ll_of(ll,sizeof ll,li->tk,li->vecn); const char *v=newtmp(c);
    emit(c,"  %s = load %s, %s %s, align %d\n",v,ll,pty,addr,type_align(li->tk,li->vecn));
    c->rvw=li->vecn>1?li->vecn:0;
    if(li->tk==T_HALF) v=h2f(c,v,li->vecn);
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
    if(vecn>1){ if(from_vw&&from_vw!=vecn){
            /* HLSL implicit truncation (D3D9): a wider float value stores into
             * a narrower slot by dropping trailing components (float4 -> float3) */
            if(from_vw>vecn&&from==VK_F32&&from_vw<=4){ static const int t3[3]={0,1,2}, t2[2]={0,1};
                char vty[24]; snprintf(vty,sizeof vty,"<%d x float>",from_vw);
                v=swizzle_read(c,v,vty,"float",vecn==3?t3:t2,vecn); from_vw=vecn; }
            else die(0,"vector width mismatch");
        }
        ValKind want=scalar_vk(tk);
        if(!from_vw){ v=coerce(c,v,from,want); if(tk==T_HALF) v=f2h(c,v,0); return splat(c,v,scalar_ll(tk),vecn); }
        if(from!=want) v=vconv(c,v,vecn,from,want);
        if(tk==T_HALF) v=f2h(c,v,vecn);
        return v; }
    if(from_vw){ if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG tost: tk=%d vecn=%d from_vw=%d line=%d\n",tk,vecn,from_vw,g_last_line); die(0,"cannot store a vector into a scalar (tk=%d vecn=%d from_vw=%d line=%d)",tk,vecn,from_vw,g_last_line); }
    const char *ev; return store_val(c,v,from,tk,&ev);
}
/* is `name` a vector type constructor (float2..uint4)? */
static int vec_name(const char *s,TypeKind *k,int *n){
    size_t l=strlen(s); if(l<2) return 0; char w=s[l-1]; if(w<'2'||w>'4') return 0;
    char base[8]; if(l-1>=sizeof base) return 0; memcpy(base,s,l-1); base[l-1]=0;
    if(!strcmp(base,"float"))*k=T_FLOAT; else if(!strcmp(base,"int"))*k=T_INT32;
    else if(!strcmp(base,"uint"))*k=T_UINT32; else if(!strcmp(base,"half"))*k=T_HALF;
    else if(!strcmp(base,"bool"))*k=T_BOOL; else return 0;
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

/* builtins table defined above expr_type_of (shared with the type checker) */
static int builtin_used[sizeof builtins/sizeof *builtins];
static void mark_builtin(const char *name){ for(size_t i=0;i<sizeof builtins/sizeof *builtins;i++) if(!strcmp(builtins[i].name,name)){ builtin_used[i]=1; return; } }
static int atomic_add_used[3];
static int tex_read_used[4], tex_write_used[4], tex_sample_used[4], tex_sample_cube_used[4], get_samp_used;
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
    static const char *names[]={"dot","cross","length","distance","normalize","reflect","clamp","mix","lerp","step","smoothstep","fract","frac","mod","fmod","radians","degrees","saturate","abs","min","max","mul","inverse","asfloat","asuint","asint"};
    int hit=0; for(size_t i=0;i<sizeof names/sizeof *names;i++) if(!strcmp(names[i],nm)){ hit=1; break; }
    if(!hit) return NULL;
    if(e->nargs<1||e->nargs>3) die(0,"%s: wrong number of arguments",nm);
    ValKind ak=VK_F32,bk=VK_F32,ck=VK_F32;
    const char *av=gen_rval(c,e->args[0],&ak); int aw=c->rvw; int rmat_a=c->rmat;
    const char *bv=NULL; int bw=0; if(e->nargs>=2){ bv=gen_rval(c,e->args[1],&bk); bw=c->rvw; }
    int rmat_b=c->rmat;
    const char *cv=NULL; int cw=0; if(e->nargs>=3){ cv=gen_rval(c,e->args[2],&ck); cw=c->rvw; }
    int rmat_c=c->rmat;
    int n=aw?aw:(bw?bw:cw); if(n==0) n=1;
    /* HLSL implicit vector truncation (D3D9): a wider float argument truncates
     * to the intrinsic's effective width, e.g. dot(float3, float4) — the
     * first argument's width wins, mirroring the E_BIN compound-assign rule. */
    if(aw>n&&ak==VK_F32){ static const int t3[3]={0,1,2}, t2[2]={0,1}; char vty[24]; snprintf(vty,sizeof vty,"<%d x float>",aw);
        av=swizzle_read(c,av,vty,"float",n==3?t3:t2,n); aw=n; }
    if(bw>n&&bk==VK_F32){ static const int t3[3]={0,1,2}, t2[2]={0,1}; char vty[24]; snprintf(vty,sizeof vty,"<%d x float>",bw);
        bv=swizzle_read(c,bv,vty,"float",n==3?t3:t2,n); bw=n; }
    if(cw>n&&ck==VK_F32){ static const int t3[3]={0,1,2}, t2[2]={0,1}; char vty[24]; snprintf(vty,sizeof vty,"<%d x float>",cw);
        cv=swizzle_read(c,cv,vty,"float",n==3?t3:t2,n); cw=n; }
    if((aw&&aw!=n)||(bw&&bw!=n)||(cw&&cw!=n)) die(0,"vector width mismatch in %s",nm);
    /* ---- HLSL intrinsics that also take integer operands ---- */
    if(!strcmp(nm,"abs")){
        if(e->nargs!=1) die(0,"abs expects 1 argument");
        if(ak==VK_I32){
            /* abs(int): select(x<0, -x, x) */
            const char *va=spread(c,av,aw,n);
            const char *neg=newtmp(c), *cmp=newtmp(c), *res=newtmp(c);
            char vty[32]; snprintf(vty,sizeof vty,"<%d x i32>",n);
            char mty[32]; snprintf(mty,sizeof mty,"<%d x i1>",n);
            if(n>1){
                emit(c,"  %s = icmp slt %s %s, zeroinitializer\n",cmp,vty,va);
                emit(c,"  %s = sub %s zeroinitializer, %s\n",neg,vty,va);
                emit(c,"  %s = select %s %s, %s %s, %s %s\n",res,mty,cmp,vty,neg,vty,va);
                *k=VK_I32; c->rvw=n; return res;
            }
            emit(c,"  %s = icmp slt i32 %s, 0\n",cmp,va);
            emit(c,"  %s = sub i32 0, %s\n",neg,va);
            emit(c,"  %s = select i1 %s, i32 %s, i32 %s\n",res,cmp,neg,va);
            *k=VK_I32; c->rvw=0; return res;
        }
        mark_builtin("fabs");
        *k=VK_F32; c->rvw=n>1?n:0;
        return elem_call(c,"air.fast_fabs.f32",spread(c,av,aw,n),n,T_FLOAT,T_FLOAT);
    }
    if(!strcmp(nm,"min")||!strcmp(nm,"max")){
        /* dispatched via the builtin table (air.fast_fmin/fmax + coercion);
         * int operands convert to float — exact for the differential range */
        if(e->nargs!=2) die(0,"%s expects 2 arguments",nm);
        mark_builtin(!strcmp(nm,"min")?"fmin":"fmax");
        return NULL; /* fall through to the table dispatch */
    }
    if(!strcmp(nm,"asfloat")||!strcmp(nm,"asuint")||!strcmp(nm,"asint")){
        /* bit reinterpret: bitcast f32<->i32, scalar or vector */
        if(e->nargs!=1) die(0,"%s expects 1 argument",nm);
        int want_f=!strcmp(nm,"asfloat");
        const char *src=(ak==VK_I32||ak==VK_U32)?"i32":"float";
        const char *dst=want_f?"float":"i32";
        char sty[48], dty[48];
        if(n>1){ snprintf(sty,sizeof sty,"<%d x %s>",n,src); snprintf(dty,sizeof dty,"<%d x %s>",n,dst); }
        else { snprintf(sty,sizeof sty,"%s",src); snprintf(dty,sizeof dty,"%s",dst); }
        const char *r=newtmp(c);
        emit(c,"  %s = bitcast %s %s to %s\n",r,sty,spread(c,av,aw,n),dty);
        *k=want_f?VK_F32:VK_I32; c->rvw=n>1?n:0; return r;
    }
    if(!strcmp(nm,"mul")){
        /* mul(a,b): vector-vector = dot; everything else reuses the binary
         * multiply (scalar, matrix*vector, vector*matrix, matrix*matrix) */
        if(e->nargs!=2) die(0,"mul expects 2 arguments");
        /* operand-order: each arg's matrix-ness captured when IT was generated */
        int am = aw ? 0 : rmat_a;
        int bm = rmat_b;
        if(aw>1&&bw>1&&!am&&!bm&&aw==bw){
            *k=VK_F32; c->rvw=0;
            return vreduce(c,vbin(c,"fmul fast",spread(c,av,aw,aw),spread(c,bv,bw,aw),aw),aw);
        }
        Expr *bin=E(E_BIN,e->line,e->col); bin->bop=B_MUL; bin->lhs=e->args[0]; bin->rhs=e->args[1];
        return gen_rval(c,bin,k);
    }
    if(!strcmp(nm,"inverse")){
        /* matrix inverse via cofactor expansion: adjugate / determinant */
        if(e->nargs!=1) die(0,"inverse expects 1 argument");
        int mn=c->rmat; if(mn<3||mn>4) die(0,"inverse supports float3x3/float4x4");
        const char *m=av;
        /* all elements: e[col][row] */
        const char *el[4][4];
        for(int cc=0;cc<mn;cc++) for(int rr=0;rr<mn;rr++) el[cc][rr]=mat_elem(c,m,mn,cc,rr);
        const char *cf[4][4];
        for(int i=0;i<mn;i++) for(int j=0;j<mn;j++){
            /* 3x3 (or 2x2) minor excluding row i, column j */
            const char *mn3[3][3]; int ri=0;
            for(int r=0;r<mn;r++){ if(r==i) continue; int ci=0;
                for(int col=0;col<mn;col++){ if(col==j) continue; mn3[ri][ci]=el[col][r]; ci++; }
                ri++; }
            const char *d;
            if(mn==4){
                const char *t1=vbin(c,"fmul fast",mn3[1][1],mn3[2][2],0);
                const char *t2=vbin(c,"fmul fast",mn3[1][2],mn3[2][1],0);
                const char *t3=vbin(c,"fsub fast",t1,t2,0);
                const char *t4=vbin(c,"fmul fast",mn3[0][0],t3,0);
                const char *t5=vbin(c,"fmul fast",mn3[1][0],mn3[2][2],0);
                const char *t6=vbin(c,"fmul fast",mn3[1][2],mn3[2][0],0);
                const char *t7=vbin(c,"fsub fast",t5,t6,0);
                const char *t8=vbin(c,"fmul fast",mn3[0][1],t7,0);
                const char *t9=vbin(c,"fsub fast",t4,t8,0);
                const char *ta=vbin(c,"fmul fast",mn3[1][0],mn3[2][1],0);
                const char *tb=vbin(c,"fmul fast",mn3[1][1],mn3[2][0],0);
                const char *tc=vbin(c,"fsub fast",ta,tb,0);
                const char *td=vbin(c,"fmul fast",mn3[0][2],tc,0);
                d=vbin(c,"fadd fast",t9,td,0);
            } else { /* 2x2: a0*b1 - a1*b0 */
                const char *t1=vbin(c,"fmul fast",mn3[0][0],mn3[1][1],0);
                const char *t2=vbin(c,"fmul fast",mn3[0][1],mn3[1][0],0);
                d=vbin(c,"fsub fast",t1,t2,0);
            }
            if((i+j)&1){ const char *f=newtmp(c); emit(c,"  %s = fsub fast float %s, %s\n",f,fconst(c,0.0),d); cf[i][j]=f; }
            else cf[i][j]=d;
        }
        /* determinant: expansion along column 0 */
        const char *det=NULL;
        for(int i=0;i<mn;i++){
            const char *p=vbin(c,"fmul fast",el[0][i],cf[i][0],0);
            det = det ? vbin(c,"fadd fast",det,p,0) : p;
        }
        /* inverse[cc][rr] = cofactor[cc][rr] / det (adjugate transpose) */
        const char *res="undef";
        for(int cc=0;cc<mn;cc++){
            const char *colv="undef";
            for(int rr=0;rr<mn;rr++){
                const char *d=vbin(c,"fdiv fast",cf[cc][rr],det,0);
                const char *ins=newtmp(c);
                emit(c,"  %s = insertelement <%d x float> %s, float %s, i32 %d\n",ins,mn,colv,d,rr);
                colv=ins;
            }
            res=mat_setcol(c,res,mn,cc,colv);
        }
        *k=VK_F32; c->rvw=0; c->rmat=mn;
        return res;
    }
    if(!strcmp(nm,"saturate")||!strcmp(nm,"abs")||!strcmp(nm,"min")||!strcmp(nm,"max")||
       !strcmp(nm,"tan")||!strcmp(nm,"asin")||!strcmp(nm,"acos")||!strcmp(nm,"atan")||!strcmp(nm,"fmod")){
        /* table-dispatched (handles scalars + per-element vectors + coercion) */
        return NULL;
    }
    /* HLSL implicitly converts int literals/args to float in math builtins
     * (dot(1, abs(N)) etc.) — coerce instead of dying */
    if(ak!=VK_F32) av=coerce(c,av,ak,VK_F32);
    if(bk!=VK_F32) bv=coerce(c,bv,bk,VK_F32);
    if(ck!=VK_F32) cv=coerce(c,cv,ck,VK_F32);
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
    if(!strcmp(nm,"mix")||!strcmp(nm,"lerp")){
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
    if(!strcmp(nm,"fract")||!strcmp(nm,"frac")){
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
    c->rvw=0; c->rmat=0; c->rstruct=NULL; /* cases set these to the width of their result, if any */
    switch(e->kind){
    case E_FCONST:{ *k=VK_F32; return fconst(c,e->fval); }
    case E_ICONST:{ *k=VK_I32; char *s=malloc(16); snprintf(s,16,"%ld",e->ival); return s; }
    case E_BOOL:{ *k=VK_I1; return e->bval?"true":"false"; }
    case E_IDENT:{
        int idx; RKind r=resolve(c,e->name,&idx);
        if(r==R_LOCAL){
            if(c->locs[idx].an>0) die(0,"cannot use an array as a value");
            if(c->locs[idx].matn){
                /* whole matrix value: aggregate load */
                char mty[32]; mll_of(mty,sizeof mty,c->locs[idx].matn,0);
                const char *v=newtmp(c);
                emit(c,"  %s = load %s, %s* %s, align %d\n",v,mty,mty,c->locs[idx].slot,mat_align(c->locs[idx].matn));
                *k=VK_F32; c->rvw=0; c->rmat=c->locs[idx].matn; return v;
            }
            LInfo li={c->locs[idx].kind,c->locs[idx].sname,0,-1,1,c->locs[idx].vecn,c->locs[idx].matn,0,0};
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
            c->rvw=cd->ty.vecn>1?cd->ty.vecn:0;
            c->rstruct=cd->ty.kind==T_STRUCT?cd->ty.struct_name:NULL;
            char ll[64];
            if(cd->ty.kind==T_STRUCT) snprintf(ll,sizeof ll,"%%struct.%s",cd->ty.struct_name);
            else ll_of(ll,sizeof ll,cd->ty.kind,cd->mut?cd->ty.vecn:0);
            const char *v=newtmp(c);
            emit(c,"  %s = load %s, %s addrspace(%d)* @_binc_%s_%s, align %d\n",v,ll,ll,cd->mut?1:2,cd->mut?"mut":"const",cd->name,type_align(cd->ty.kind,0));
            if(cd->ty.kind==T_HALF){ const char *w=newtmp(c); emit(c,"  %s = fpext half %s to float\n",w,v); v=w; }
            return v; }
        if(r==R_SCALAR){ Param *p=&c->fn->params[idx]; *k=scalar_vk(p->ty.kind); c->rvw=p->ty.vecn>1?p->ty.vecn:0;
            if(p->ty.matn){ /* matrix by-value param: the AIR value is the aggregate */
                char nm[64]; snprintf(nm,sizeof nm,"%%_%s",p->name);
                c->rmat=p->ty.matn; return strdup(nm);
            }
            if(p->ty.kind==T_STRUCT&&!p->ty.is_ptr&&!p->ty.matn){
                /* struct by-value param: the AIR value IS the aggregate
                 * (GetPrimitiveData(FMaterialVertexParameters Parameters)) */
                c->rstruct=p->ty.struct_name;
                char *nm=malloc(strlen(p->name)+3); snprintf(nm,strlen(p->name)+3,"%%_%s",p->name);
                return nm;
            }
            if(!c->scalar_load[idx]){
                if(c->fn->is_kernel){ char ll[32]; ll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn); const char *v=newtmp(c);
                    emit(c,"  %s = load %s, %s addrspace(2)* %%_%s, align %d\n",v,ll,ll,p->name,type_align(p->ty.kind,p->ty.vecn));
                    if(p->ty.kind==T_HALF) v=h2f(c,v,p->ty.vecn);
                    c->scalar_load[idx]=(char*)v;
                } else { char *nm=malloc(strlen(p->name)+3); snprintf(nm,strlen(p->name)+3,"%%_%s",p->name);
                    if(p->ty.kind==T_HALF){ const char *w=h2f(c,nm,p->ty.vecn); nm=(char*)w; }
                    c->scalar_load[idx]=nm; } }
            return c->scalar_load[idx]; }
        die(0,"undefined name %s",e->name);
    }
    case E_DEREF: case E_FIELD: case E_INDEX:{
        /* Coordinate properties are built-ins rather than memory fields. The explicit
         * coordinate itself is the global position; local/group values are appended as
         * hidden built-in arguments when referenced. */
        if(e->kind==E_FIELD){
            if(getenv("BINC_DEBUG_D3D9")){ int dvi; RKind dvr=e->operand->kind==E_IDENT?resolve(c,e->operand->name,&dvi):R_NONE; const char *iname=e->operand->kind==E_FIELD?(e->operand->operand&&e->operand->operand->kind==E_IDENT?e->operand->operand->name:"(nested)"):""; fprintf(stderr,"DBG fld: opk=%d fld=%s vr=%d inner=%s iname=%s\n",e->operand->kind,e->field,dvr,e->operand->kind==E_FIELD?(e->operand->field?e->operand->field:"?"):"",iname); }
            /* field access on a VALUE (computed vector/struct result): vector
             * swizzle for vector results, extractvalue for struct results */
            if(getenv("BINC_DEBUG_IW")&&e->operand->kind==E_IDENT){ int dvi; RKind dvr=resolve(c,e->operand->name,&dvi); fprintf(stderr,"DBG fldsel: %s.%s vr=%d vi=%d\n",e->operand->name,e->field,dvr,dvi); }
            if(e->operand->kind==E_ICONST||e->operand->kind==E_FCONST||e->operand->kind==E_BOOL){
                /* scalar-constant swizzle: `1u.xxx` — splat the constant to the
                 * swizzle width (a scalar broadcasts identically to every slot) */
                int idxs[4]; int nc=swizzle_idx(e->field,idxs);
                if(nc<0) die(0,"invalid vector component .%s",e->field);
                ValKind ck; const char *cv=gen_rval(c,e->operand,&ck);
                const char *elt=ck==VK_F32?"float":"i32";
                const char *sp=splat(c,cv,elt,nc);
                char vty[32]; snprintf(vty,sizeof vty,"<%d x %s>",nc,elt);
                *k=ck; c->rvw=nc;
                return swizzle_read(c,sp,vty,elt,idxs,nc);
            }
            if(e->operand->kind!=E_IDENT&&e->operand->kind!=E_DEREF&&e->operand->kind!=E_FIELD&&e->operand->kind!=E_INDEX){
                ValKind ck; const char *cv=gen_rval(c,e->operand,&ck);
                if(c->rmat>0&&e->field[0]=='_'&&e->field[1]=='m'){
                    /* matrix element swizzle: M._m10_m11_m12 (row N, col M) */
                    int n=0, ir[4], ic[4];
                    const char *p=e->field+1;
                    while(*p){
                        if(*p=='_') p++;
                        if(p[0]=='m'&&p[1]>='0'&&p[1]<='4'&&p[2]>='0'&&p[2]<='4'){ ir[n]=p[1]-'0'; ic[n]=p[2]-'0'; n++; p+=3; }
                        else break;
                    }
                    if(n>0){
                        if(n==1){ *k=VK_F32; c->rvw=0; return mat_elem(c,cv,c->rmat,ic[0],ir[0]); }
                        const char *acc="undef";
                        for(int i=0;i<n;i++){ const char *e2=mat_elem(c,cv,c->rmat,ic[i],ir[i]);
                            const char *ins=newtmp(c);
                            emit(c,"  %s = insertelement <%d x float> %s, float %s, i32 %d\n",ins,n,acc,e2,i); acc=ins; }
                        *k=VK_F32; c->rvw=n; return acc;
                    }
                }
                if(c->rvw>1){
                    /* vector call result: asfloat(...).x — swizzle */
                    int idxs[4]; int nc=swizzle_idx(e->field,idxs);
                    if(nc<0) die(0,"invalid vector component .%s",e->field);
                    const char *elt=ck==VK_I32?"i32":"float";
                    char vty[32]; snprintf(vty,sizeof vty,"<%d x %s>",c->rvw,elt);
                    *k=ck;
                    if(nc==1){ const char *r=newtmp(c); c->rvw=0;
                        emit(c,"  %s = extractelement %s %s, i32 %d\n",r,vty,cv,idxs[0]); return r; }
                    c->rvw=nc;
                    return swizzle_read(c,cv,vty,elt,idxs,nc);
                }
                if(!c->rstruct){
                    /* scalar call-result swizzle: f().xxx — splat to the swizzle width */
                    int idxs[4]; int nc=swizzle_idx(e->field,idxs);
                    if(nc>0){
                        const char *elt=ck==VK_I32?"i32":"float";
                        if(nc==1){ *k=ck; c->rvw=0; return cv; }
                        const char *sp=splat(c,cv,elt,nc);
                        *k=ck; c->rvw=nc;
                        return sp;
                    }
                    die(0,"field access on a non-struct value");
                }
                StructDef *sd=find_struct(c->prog,c->rstruct);
                int fi=-1; for(size_t i=0;i<sd->nfields;i++) if(!strcmp(sd->fields[i].name,e->field)){ fi=(int)i; break; }
                if(fi<0) die(0,"no field .%s on %s",e->field,c->rstruct);
                Field *fd=&sd->fields[fi];
                const char *ev=newtmp(c);
                char fl[32]; type_ll(fl,sizeof fl,fd->ty.kind,fd->ty.struct_name,fd->ty.vecn);
                emit(c,"  %s = extractvalue %%struct.%s %s, %d\n",ev,c->rstruct,cv,fi);
                if(fd->ty.kind==T_HALF) ev=h2f(c,ev,fd->ty.vecn);
                *k=scalar_vk(fd->ty.kind); c->rvw=fd->ty.vecn>1?fd->ty.vecn:0;
                c->rstruct = fd->ty.kind==T_STRUCT?fd->ty.struct_name:NULL;
                return ev;
            }
            if(e->operand->kind==E_IDENT){
                int vi; RKind vr=resolve(c,e->operand->name,&vi);
                if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG fldrv: %s.%s vr=%d vi=%d lv=%d fn=%s np=%zu\n",e->operand->name,e->field,vr,vi,vr==R_LOCAL?c->locs[vi].vecn:(vr==R_SCALAR?c->fn->params[vi].ty.vecn:0),c->fn->name,c->fn->nparams);
                if(vr==R_SCALAR && c->fn->params[vi].ty.vecn>1){
                    Param *vp=&c->fn->params[vi];
                    int idxs[4]; int nc=swizzle_idx(e->field,idxs);
                    if(nc<0) die(0,"invalid vector component .%s",e->field);
                    for(int i=0;i<nc;i++) if(idxs[i]>=vp->ty.vecn) die(0,"invalid vector component .%s",e->field);
                    char nmbuf[64]; snprintf(nmbuf,sizeof nmbuf,"%%_%s",vp->name);
                    const char *nm=nmbuf;
                    const char *elt = vp->ty.kind==T_HALF?"float":scalar_ll(vp->ty.kind);
                    if(vp->ty.kind==T_HALF){
                        if(!c->scalar_load[vi]){ const char *w=h2f(c,nm,vp->ty.vecn); c->scalar_load[vi]=(char*)w; }
                        nm=c->scalar_load[vi];
                    }
                    char vty[32]; snprintf(vty,sizeof vty,"<%d x %s>",vp->ty.vecn,elt);
                    *k=scalar_vk(vp->ty.kind);
                    if(nc==1){ const char *r=newtmp(c); c->rvw=0;
                        emit(c,"  %s = extractelement %s %s, i32 %d\n",r,vty,nm,idxs[0]); return r; }
                    c->rvw=nc;
                    return swizzle_read(c,nm,vty,elt,idxs,nc);
                }
                if(vr==R_LOCAL && c->locs[vi].vecn>1){
                    int idxs[4]; int nc=swizzle_idx(e->field,idxs);
                    if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG fldloc: %s.%s nc=%d lv=%d\n",e->operand->name,e->field,nc,c->locs[vi].vecn);
                    if(nc<0) die(0,"invalid vector component .%s",e->field);
                    for(int i=0;i<nc;i++) if(idxs[i]>=c->locs[vi].vecn) die(0,"invalid vector component .%s",e->field);
                    if(nc>1){
                        LInfo li={c->locs[vi].kind,c->locs[vi].sname,0,-1,1,c->locs[vi].vecn,c->locs[vi].matn,0,0};
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
                if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG gensw: field=%s nc=%d opk=%d\n",e->field,nc,e->operand->kind);
                if(nc>1){
                    LInfo li; char *addr=gen_lval(c,e->operand,&li,0);
                    int fv=li.vecn>1?li.vecn:(li.an>0&&li.an<=4?li.an:0); /* packed cbuffer vectors: [3 x float] field swizzles */
                    if(fv<=1){
                        /* scalar base swizzle: float f; f.xxx — splat the scalar */
                        if(li.vecn==0&&li.matn==0&&!li.an){
                            ValKind lk; const char *lv=emit_load_t(c,&li,addr,&lk);
                            const char *elt=li.tk==T_HALF?"float":scalar_ll(li.tk);
                            const char *sp=splat(c,lv,elt,nc);
                            *k=scalar_vk(li.tk); c->rvw=nc;
                            return sp;
                        }
                        die(0,"swizzle on a non-vector value");
                    }
                    for(int i=0;i<nc;i++) if(idxs[i]>=fv) die(0,"invalid vector component .%s",e->field);
                    ValKind lk; const char *lv=emit_load_t(c,&li,addr,&lk);
                    const char *elt = li.tk==T_HALF?"float":scalar_ll(li.tk);
                    char vty[32]; snprintf(vty,sizeof vty,"<%d x %s>",fv,elt);
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
        if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG fldfin: %s vecn=%d line=%d\n",e->field,li.vecn,e->line);
        if(getenv("BINC_DEBUG_IW")&&e->operand->kind==E_IDENT) fprintf(stderr,"DBG fld: %s.%s li(vecn=%d,tk=%d,as=%d)\n",e->operand->name,e->field,li.vecn,li.tk,li.as);
        if(li.pi>=0) c->read[li.pi]=1;
        if(li.as==AS_CONSTANT&&li.matn>0){ /* D3D cbuffer matrices are row-major */
            ValKind lk; const char *v=emit_load_t(c,&li,addr,&lk);
            return mat_transpose(c,v,li.matn,k);
        }
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
        Type bt=bind_ty(c->fn,e->cty); Type *t=&bt; TypeKind tk=t->kind; int tv=t->vecn>1?t->vecn:0;
        if(t->matn){ /* matrix cast: same-size identity, larger->smaller = top-left submatrix */
            if(!c->rmat) die(0,"matrix cast requires a matrix operand");
            if(c->rmat==t->matn){ *k=VK_F32; c->rvw=0; c->rmat=t->matn; return v; }
            if(c->rmat<t->matn) die(0,"matrix cast cannot widen");
            const char *res="undef";
            for(int cc=0;cc<t->matn;cc++){
                const char *colv="undef";
                for(int rr=0;rr<t->matn;rr++){
                    const char *me=mat_elem(c,v,c->rmat,cc,rr);
                    const char *ins=newtmp(c);
                    emit(c,"  %s = insertelement <%d x float> %s, float %s, i32 %d\n",ins,t->matn,colv,me,rr);
                    colv=ins;
                }
                res=mat_setcol(c,res,t->matn,cc,colv);
            }
            *k=VK_F32; c->rvw=0; c->rmat=t->matn; return res;
        }
        if(tk==T_BOOL){
            if(vw) die(0,"cannot cast a vector to bool");
            const char *r=newtmp(c); *k=VK_I1; c->rvw=0;
            if(lk==VK_F32) emit(c,"  %s = fcmp one float %s, 0.000000e+00\n",r,v);
            else emit(c,"  %s = icmp ne i32 %s, 0\n",r,v);
            return r;
        }
        ValKind want=scalar_vk(tk);
        if(tk==T_STRUCT){ /* (Struct)0 — zero-initialized literal struct */
            if(lk!=VK_I32||vw) die(0,"struct cast requires a scalar integer (0)");
            *k=VK_I32; c->rvw=0; c->rstruct=t->struct_name;
            return "zeroinitializer";
        }
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
        if(getenv("BINC_DEBUG_IW")&&!strcmp(c->fn->name,"GetPrimitive_PerObjectGBufferData_FromFlags")) fprintf(stderr,"DBG bin: bop=%d lk=%d lw=%d lh=%d\n",e->bop,lk,lw,e->lhs->kind);
        const char *r=gen_rval(c,e->rhs,&rk); int rw=c->rvw; int rm=c->rmat;
        /* shift counts mask to 5 bits (C/CPU semantics; LLVM shifts by >=width are poison) */
        if((e->bop==B_SHL||e->bop==B_SHR)&&!lm&&!rm&&lk!=VK_F32&&rk!=VK_F32&&!lw&&!rw){
            const char *mc=newtmp(c); emit(c,"  %s = and i32 %s, 31\n",mc,r); r=mc; }
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
            if(e->bop==B_MUL&&lw>1&&lw<mn&&rk==VK_F32){
                /* D3D9: mul(float3, float4x4) — implicitly extend the vector
                 * to float4 (w=1); the result truncates back to float3 at the
                 * store (plain-assign truncation) */
                const char *ext=newtmp(c);
                emit(c,"  %s = shufflevector <%d x float> %s, <%d x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 poison>\n",ext,lw,l,lw);
                const char *w1=fconst(c,1.0);
                const char *ext2=newtmp(c);
                emit(c,"  %s = insertelement <4 x float> %s, float %s, i32 3\n",ext2,ext,w1);
                l=ext2; lw=mn;
            }
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
        if(lw&&rw&&lw!=rw){
            /* HLSL implicit truncation: `float3 += float4 * s` keeps the lhs width */
            if(lw<rw){ static const int t3[3]={0,1,2}, t2[2]={0,1};
                if(rk==VK_F32){ const char *tr=swizzle_read(c,r,"<4 x float>","float",lw==3?t3:t2,lw); r=tr; }
                else { const char *rr=newtmp(c); emit(c,"  %s = shufflevector <%d x i32> %s, <%d x i32> poison, i32 0, i32 1%s\n",rr,rw,r,rw,lw==3?", i32 2":""); r=rr; }
                rw=lw;
            } else { static const int t3[3]={0,1,2}, t2[2]={0,1};
                if(lk==VK_F32){ const char *tr=swizzle_read(c,l,"<4 x float>","float",rw==3?t3:t2,rw); l=tr; }
                else { const char *lr=newtmp(c); emit(c,"  %s = shufflevector <%d x i32> %s, <%d x i32> poison, i32 0, i32 1%s\n",lr,lw,l,lw,rw==3?", i32 2":""); l=lr; }
                lw=rw;
            }
            vw=lw?lw:rw;
        }
        if(getenv("BINC_DEBUG_D3D9")) fprintf(stderr,"DBG binop %s: lw=%d rw=%d\n",bin_op(e->bop,isf,uns),lw,rw);
        const char *op=bin_op(e->bop,isf,uns);
        if(vw){ const char *elt=ok==VK_F32?"float":"i32";
            if(!lw){ l=coerce(c,l,lk,ok); l=splat(c,l,elt,vw); } else if(lk!=ok) l=vconv(c,l,lw,lk,ok);
            if(!rw){ r=coerce(c,r,rk,ok); r=splat(c,r,elt,vw); } else if(rk!=ok) r=vconv(c,r,rw,rk,ok);
            const char *v=newtmp(c); *k=ok; c->rvw=vw;
            emit(c,"  %s = %s <%d x %s> %s, %s\n",v,op,vw,elt,l,r); return v; }
        if(isf){ l=coerce(c,l,lk,VK_F32); r=coerce(c,r,rk,VK_F32); }
        const char *v=newtmp(c); *k=ok;
        emit(c,"  %s = %s %s %s, %s\n",v,op,isf?"float":"i32",l,r); return v; }
    case E_CMP:{ ValKind lk,rk; const char *l=gen_rval(c,e->lhs,&lk); int lw=c->rvw; int lm=c->rmat;
        const char *r=gen_rval(c,e->rhs,&rk); int rw=c->rvw; int rm=c->rmat;
        if(c->rstruct||lm||rm) die(0,"cannot compare struct or matrix values");
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
    case E_LOG:{ ValKind lk; const char *l=gen_rval(c,e->lhs,&lk);
        if(c->rvw) die(0,"vector operand in logical operator");
        if(lk!=VK_I1){ if(lk==VK_I32||lk==VK_U32){ const char *n=newtmp(c); emit(c,"  %s = icmp ne i32 %s, 0\n",n,l); l=n; } else die(0,"logical operands must be bool"); }
        /* short-circuit && / || via control flow (C semantics) */
        int p0=c->curbb, eval=newlbl(c), done=newlbl(c);
        if(e->log==L_AND) emit(c,"  br i1 %s, label %%bb%d, label %%bb%d\n",l,eval,done);
        else emit(c,"  br i1 %s, label %%bb%d, label %%bb%d\n",l,done,eval);
        lbl(c,eval);
        ValKind rk; const char *r=gen_rval(c,e->rhs,&rk);
        if(c->rvw) die(0,"vector operand in logical operator");
        if(rk!=VK_I1){ if(rk==VK_I32||rk==VK_U32){ const char *n=newtmp(c); emit(c,"  %s = icmp ne i32 %s, 0\n",n,r); r=n; } else die(0,"logical operands must be bool"); }
        emit(c,"  br label %%bb%d\n",done);
        lbl(c,done);
        const char *v=newtmp(c); *k=VK_I1;
        emit(c,"  %s = phi i1 [ %s, %%bb%d ], [ %s, %%bb%d ]\n",v,r,eval,e->log==L_AND?"false":"true",p0);
        return v; }
    case E_INCDEC:{ /* postfix ++/--: load, add/sub 1, store; value = the old value */
        ValKind ck;
        LInfo li; char *addr=gen_lval(c,e->operand,&li,1);
        if(li.vecn||li.an||li.matn||li.tk==T_STRUCT) die(0,"cannot increment/decrement this value");
        const char *cur=emit_load_t(c,&li,addr,&ck);
        if(li.pi>=0) c->read[li.pi]=1;
        const char *one = ck==VK_F32?fconst(c,1.0):"1";
        const char *nv=newtmp(c);
        emit(c,"  %s = %s %s %s, %s\n",nv,ck==VK_F32?"fadd fast":"add",ck==VK_F32?"float":"i32",cur,one);
        const char *sv=store_val(c,nv,ck,li.tk,&nv);
        char pty[96]; pty_str(pty,sizeof pty,li.tk,li.sname,li.as,li.is_local,li.vecn,li.matn);
        char ll[32]; ll_of(ll,sizeof ll,li.tk,li.vecn);
        emit(c,"  store %s %s, %s %s, align %d\n",ll,sv,pty,addr,type_align(li.tk,li.vecn));
        *k=ck; c->rvw=0; return cur; }
    case E_ASSIGN:{
        /* v.xy += rhs -> v.xy = v.xy + rhs (compound swizzle desugar; the base
         * may itself be a lowered field like `__in.v`, so copy the operand tree) */
        if(e->aop!=A_ASSIGN && e->operand->kind==E_FIELD){
            int idxs[4]; int nc=swizzle_idx(e->operand->field,idxs);
            if(nc>1){
                BinOp bop = e->aop==A_ADDEQ?B_ADD:e->aop==A_SUBEQ?B_SUB:e->aop==A_MULEQ?B_MUL:e->aop==A_DIVEQ?B_DIV:B_ADD;
                Expr *fd=E(E_FIELD,e->operand->line,e->operand->col);
                fd->operand=rw_copy(e->operand->operand); fd->field=strdup(e->operand->field);
                Expr *sum=E(E_BIN,e->line,e->col); sum->bop=bop; sum->lhs=fd; sum->rhs=e->rhs;
                e->aop=A_ASSIGN; e->rhs=sum;
            }
        }
        /* swizzle assignment: v.xy = rhs — write each named component */
        if(e->aop==A_ASSIGN && e->operand->kind==E_FIELD){
            /* swizzle assignment v.xy = rhs (also on struct fields: s.f.rgb = v) */
            Expr *root=e->operand->operand;
            while(root->kind==E_FIELD||root->kind==E_INDEX) root=root->operand;
            int wi; RKind wr = root->kind==E_IDENT ? resolve(c,root->name,&wi) : R_NONE;
            int root_vecn = 0;
            if(wr==R_LOCAL) root_vecn=c->locs[wi].vecn;
            int idxs[4]; int nc=swizzle_idx(e->operand->field,idxs);
            if(nc>1){
                LInfo lb; char *base;
                if(root->kind==E_IDENT&&wr==R_LOCAL&&e->operand->operand->kind==E_IDENT){ base=c->locs[wi].slot; lb=(LInfo){c->locs[wi].kind,c->locs[wi].sname,0,-1,1,c->locs[wi].vecn,c->locs[wi].matn,0,0}; }
                else { base=gen_lval(c,e->operand->operand,&lb,0); }
                int base_vecn = root_vecn>1 ? root_vecn : lb.vecn;
                if(base_vecn>1){ for(int i=0;i<nc;i++) if(idxs[i]>=base_vecn) die(0,"invalid vector component .%s",e->operand->field); }
                ValKind rk; const char *rv=gen_rval(c,e->rhs,&rk); int rw=c->rvw;
                if(rw<nc) die(0,"swizzle assignment width mismatch");
                char pty[96]; pty_str(pty,sizeof pty,lb.tk,NULL,lb.as,lb.is_local,base_vecn,0);
                char *bit=newtmp(c);
                emit(c,"  %s = bitcast %s %s to %s*\n",bit,pty,base,lb.tk==T_HALF?"half":"float");
                for(int i=0;i<nc;i++){
                    const char *p=newtmp(c);
                    emit(c,"  %s = getelementptr inbounds %s, %s* %s, i64 %d\n",p,lb.tk==T_HALF?"half":"float",lb.tk==T_HALF?"half":"float",bit,idxs[i]);
                    const char *ev=newtmp(c);
                    emit(c,"  %s = extractelement <%d x float> %s, i32 %d\n",ev,rw,rv,i);
                    const char *sv=ev;
                    if(lb.tk==T_HALF){ const char *h=newtmp(c); emit(c,"  %s = fptrunc float %s to half\n",h,ev); sv=h; }
                    emit(c,"  store %s %s, %s* %s, align %d\n",lb.tk==T_HALF?"half":"float",sv,lb.tk==T_HALF?"half":"float",p,lb.tk==T_HALF?2:4);
                }
                *k=scalar_vk(lb.tk); c->rvw=nc; return rv;
            }
        }
        ValKind rk; const char *rhs=gen_rval(c,e->rhs,&rk); int rw=c->rvw; int rm=c->rmat;
        LInfo li; char *addr=gen_lval(c,e->operand,&li,1);
        if(rm){
            if(e->aop!=A_ASSIGN) die(0,"compound assignment on a matrix");
            if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG matassign: lhs=%s li.matn=%d rm=%d rstruct=%s\n",e->operand->kind==E_FIELD?e->operand->field:"?",li.matn,rm,c->rstruct?c->rstruct:"(null)");
            if(li.matn!=rm) die(0,"matrix width mismatch in assignment");
            char mty[32]; mll_of(mty,sizeof mty,rm,0);
            emit(c,"  store %s %s, %s* %s, align %d\n",mty,rhs,mty,addr,mat_align(rm));
            *k=VK_F32; c->rvw=0; c->rmat=rm; return rhs;
        }
        if(li.tk==T_STRUCT){
            if(e->aop!=A_ASSIGN) die(0,"compound assignment on a struct");
            if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG structassign: lhs=%s rstruct=%s want=%s\n",e->operand->kind==E_FIELD?e->operand->field:"?",c->rstruct?c->rstruct:"(null)",li.sname?li.sname:"(null)");
            if(!c->rstruct||strcmp(c->rstruct,li.sname)) die(0,"struct type mismatch in assignment");
            char sn[96]; snprintf(sn,sizeof sn,"%%struct.%s",li.sname);
            char sp[96];
            if(li.as) snprintf(sp,sizeof sp,"%%struct.%s addrspace(%d)*",li.sname,li.as);
            else snprintf(sp,sizeof sp,"%%struct.%s*",li.sname);
            int sal; StructDef *sd=find_struct(c->prog,li.sname);
            if(!sd) die(0,"unknown struct %s",li.sname);
            struct_layout(sd,&sal);
            emit(c,"  store %s %s, %s %s, align %d\n",sn,rhs,sp,addr,sal);
            *k=VK_I32; c->rvw=0; c->rstruct=li.sname; return rhs;
        }
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
                if(rw&&rw!=vw){ /* HLSL implicit truncation: float3 += float4 keeps lhs width */
                    if(rw>vw){ static const int t3[3]={0,1,2}, t2[2]={0,1};
                        if(rk==VK_F32) rhs=swizzle_read(c,rhs,"<4 x float>","float",vw==3?t3:t2,vw);
                        else { const char *rr=newtmp(c); emit(c,"  %s = shufflevector <%d x i32> %s, <%d x i32> poison, i32 0, i32 1%s\n",rr,rw,rhs,rw,vw==3?", i32 2":""); rhs=rr; }
                        rw=vw;
                    } else { static const int t3[3]={0,1,2}, t2[2]={0,1};
                        if(ck==VK_F32) cur=swizzle_read(c,cur,"<4 x float>","float",rw==3?t3:t2,rw);
                        else { const char *cr=newtmp(c); emit(c,"  %s = shufflevector <%d x i32> %s, <%d x i32> poison, i32 0, i32 1%s\n",cr,vw,cur,vw,rw==3?", i32 2":""); cur=cr; }
                        vw=rw;
                    }
                }
                if(ck!=ok) cur=vconv(c,cur,vw,ck,ok);
                if(!rw){ rhs=coerce(c,rhs,rk,ok); rhs=splat(c,rhs,elt,vw); } else if(rk!=ok) rhs=vconv(c,rhs,rw,rk,ok);
                emit(c,"  %s = %s <%d x %s> %s, %s\n",nv,as_op(e->aop,isf,uns),vw,elt,cur,rhs); rw=vw;
            } else { if(isf){ cur=coerce(c,cur,ck,VK_F32); rhs=coerce(c,rhs,rk,VK_F32); }
                emit(c,"  %s = %s %s %s, %s\n",nv,as_op(e->aop,isf,uns),isf?"float":"i32",cur,rhs); }
            rhs=nv; rk=ok;
        }
        const char *sv, *ev;
        warn_implicit(c,e->rhs,rk,li.tk,vw);
        if(vw){ if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG stor-A: vw=%d rw=%d tk=%d line=%d\n",vw,rw,li.tk,e->line); sv=to_storage(c,rhs,rk,rw,li.tk,vw); ev=sv; }
        else { if(rw) die(0,"cannot store a vector into a scalar"); sv=store_val(c,rhs,rk,li.tk,&ev); }
        char pty[96]; pty_str(pty,sizeof pty,li.tk,li.sname,li.as,li.is_local,vw,li.matn);
        char ll[32]; ll_of(ll,sizeof ll,li.tk,vw);
        if(getenv("BINC_DEBUG_ASSIGN")) fprintf(stderr,"DBG assign: rhs=%s rk=%d rw=%d vw=%d tk=%d\n",rhs,rk,rw,vw,li.tk);
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
        ValKind ok=isf?VK_F32:((ak==VK_I1&&bk==VK_I1)?VK_I1:((ak==VK_U32||bk==VK_U32)?VK_U32:VK_I32));
        av=coerce(c,av,ak,ok); bv=coerce(c,bv,bk,ok);
        const char *elt=ok==VK_F32?"float":(ok==VK_I1?"i1":"i32");
        const char *r=newtmp(c); *k=ok; c->rvw=vw;
        if(vw){ if(!aw) av=splat(c,av,elt,vw); if(!bw) bv=splat(c,bv,elt,vw);
            const char *mask=splat(c,cv,"i1",vw);
            char ty[32], mty[32]; ll_of(ty,sizeof ty,ok==VK_F32?T_FLOAT:T_INT32,vw);
            snprintf(mty,sizeof mty,"<%d x i1>",vw);
            emit(c,"  %s = select %s %s, %s %s, %s %s\n",r,mty,mask,ty,av,ty,bv); }
        else emit(c,"  %s = select i1 %s, %s %s, %s %s\n",r,cv,elt,av,elt,bv);
        return r; }
    case E_CALL:{
        if(getenv("BINC_DEBUG_IW")&&!strcmp(e->name,"CondMask")&&!strcmp(c->fn->name,"GetPrimitive_PerObjectGBufferData_FromFlags")) fprintf(stderr,"DBG cm-call: nargs=%zu line=%d\n",e->nargs,e->line);
        if(e->callee && e->callee->kind==E_FIELD){
            /* texture methods: tex.read(c), tex.write(v, c), tex.sample(smp, uv) */
            if(e->callee->operand->kind==E_IDENT){
                int ti; RKind tr=resolve(c,e->callee->operand->name,&ti);
                if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG meth: %s.%s tr=%d fn=%s\n",e->callee->operand->name,e->name,tr,c->fn->name);
                if(tr==R_TEXTURE){
                    Param *tp=&c->fn->params[ti];
                    const char *elt,*vec,*suf,*an; tex_kinds(tp->ty.tex_elt,&elt,&vec,&suf,&an);
                    (void)elt; (void)an;
                    char tname[64]; snprintf(tname,sizeof tname,"%%_%s",tp->name);
                    if(!strcmp(e->name,"read")||!strcmp(e->name,"Load")){
                        /* HLSL Load: int2 (or int3 with a mip) coordinate */
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
                    if(!strcmp(e->name,"sample")||!strcmp(e->name,"Sample")||!strcmp(e->name,"SampleLevel")){
                        if(e->nargs!=2&&e->nargs!=3) die(0,"texture sample expects 2 arguments (sampler, uv) or 3 (sampler, uv, lod)");
                        int si; if(e->args[0]->kind!=E_IDENT||resolve(c,e->args[0]->name,&si)!=R_SAMPLER)
                            die(0,"texture sample's first argument must be a sampler parameter");
                        ValKind uk; const char *uv=gen_rval(c,e->args[1],&uk);
                        if(tp->ty.tex_cube){ if(c->rvw!=3) die(0,"cube sample direction must be a float3"); }
                        else if(c->rvw!=2) die(0,"texture sample uv must be a float2");
                        const char *lod=fconst(c,0.0);
                        if(e->nargs==3){ ValKind lk; lod=gen_rval(c,e->args[2],&lk);
                            if(c->rvw) die(0,"texture sample lod must be a scalar"); }
                        tex_sample_used[tp->ty.tex_elt==T_HALF?1:tp->ty.tex_elt==T_INT32?2:tp->ty.tex_elt==T_UINT32?3:0]=1;
                        if(tp->ty.tex_cube) tex_sample_cube_used[tp->ty.tex_elt==T_HALF?1:tp->ty.tex_elt==T_INT32?2:tp->ty.tex_elt==T_UINT32?3:0]=1;
                        char sname[64]; snprintf(sname,sizeof sname,"%%_%s",e->args[0]->name);
                        const char *r=newtmp(c);
                        if(tp->ty.tex_cube){
                            /* probed: air.sample_texture_cube.<suf>(tex, samp, <3 x float>, i1, lod, grad, i32) */
                            emit(c,"  %s = call { %s, i8 } @air.sample_texture_cube.%s(%%struct._texture_2d_t addrspace(1)* %s, %%struct._sampler_t addrspace(2)* %s, <3 x float> %s, i1 false, float %s, float %s, i32 0)\n",r,vec,suf,tname,sname,uv,lod,fconst(c,0.0));
                        } else {
                            emit(c,"  %s = call { %s, i8 } @air.sample_texture_2d.%s(%%struct._texture_2d_t addrspace(1)* %s, %%struct._sampler_t addrspace(2)* %s, <2 x float> %s, i1 true, <2 x i32> zeroinitializer, i1 false, float %s, float %s, i32 0)\n",r,vec,suf,tname,sname,uv,lod,fconst(c,0.0));
                        }
                        const char *v=newtmp(c);
                        emit(c,"  %s = extractvalue { %s, i8 } %s, 0\n",v,vec,r);
                        if(tp->ty.tex_elt==T_HALF){ const char *w=newtmp(c); emit(c,"  %s = fpext <4 x half> %s to <4 x float>\n",w,v); v=w; }
                        *k=VK_F32; c->rvw=4; return v;
                    }
                    die(0,"unknown texture method .%s (use read, write, sample)",e->name);
                } else if(tr==R_PTR&&e->name&&(!strncmp(e->name,"Load",4)||!strncmp(e->name,"Store",5))){
                    /* ByteAddressBuffer.Load(byteOff) / Load2..4 / Store(v, off)
                     * — the buffer is an i32 device pointer; view it as i8*,
                     * GEP by the byte offset, reload as i32 (or a vector) */
                    Param *bp=&c->fn->params[ti];
                    int is_store = !strncmp(e->name,"Store",5);
                    int nw = !strncmp(e->name,"Load",4) ? (strlen(e->name)>4?(int)(e->name[4]-'0'):1) : 0;
                    if(!is_store&&(nw<1||nw>4)) die(0,"unsupported buffer method .%s",e->name);
                    if(e->nargs!=(is_store?2:1)) die(0,"buffer %s expects (%s)",e->name,is_store?"address, value":"byteOffset");
                    /* HLSL: Load(byteOffset) / Store(address, value) — the
                     * address is ALWAYS args[0]; the value is args[1] */
                    ValKind ok; const char *ov=gen_rval(c,e->args[0],&ok);
                    if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG buf: %s off rvw=%d kind=%d\n",e->name,c->rvw,ok);
                    if(c->rvw) die(0,"buffer %s offset must be a scalar",e->name);
                    if(ok!=VK_I32&&ok!=VK_U32) die(0,"buffer %s offset must be an integer",e->name);
                    if(ok==VK_I32){ const char *o64=newtmp(c); emit(c,"  %s = sext i32 %s to i64\n",o64,ov); ov=o64; }
                    else if(ok==VK_U32){ const char *o64=newtmp(c); emit(c,"  %s = zext i32 %s to i64\n",o64,ov); ov=o64; }
                    c->read[ti]=1;
                    char *base=newtmp(c);
                    emit(c,"  %s = bitcast %s addrspace(%d)* %%_%s to i8 addrspace(%d)*\n",base,
                         scalar_ll(bp->ty.kind),bp->ty.as,bp->name,bp->ty.as);
                    char *gp=newtmp(c);
                    emit(c,"  %s = getelementptr inbounds i8, i8 addrspace(%d)* %s, i64 %s\n",gp,bp->ty.as,base,ov);
                    if(nw>1){
                        /* multi-word load: read nw consecutive i32s */
                        char *v32=newtmp(c);
                        emit(c,"  %s = bitcast i8 addrspace(%d)* %s to <4 x i32> addrspace(%d)*\n",v32,bp->ty.as,gp,bp->ty.as);
                        char *w=newtmp(c);
                        emit(c,"  %s = load <4 x i32>, <4 x i32> addrspace(%d)* %s, align 4\n",w,bp->ty.as,v32);
                        char *sw=newtmp(c);
                        emit(c,"  %s = shufflevector <4 x i32> %s, <4 x i32> undef, <4 x i32> <i32 0, i32 1, i32 2, i32 3>\n",sw,w);
                        const char *r=newtmp(c);
                        if(nw==2) emit(c,"  %s = shufflevector <4 x i32> %s, <4 x i32> undef, <2 x i32> <i32 0, i32 1>\n",r,sw);
                        else if(nw==3) emit(c,"  %s = shufflevector <4 x i32> %s, <4 x i32> undef, <3 x i32> <i32 0, i32 1, i32 2>\n",r,sw);
                        else emit(c,"  %s = shufflevector <4 x i32> %s, <4 x i32> undef, <4 x i32> <i32 0, i32 1, i32 2, i32 3>\n",r,sw);
                        *k=VK_U32; c->rvw=nw; return r;
                    }
                    /* single-word Load / Store */
                    if(!strncmp(e->name,"Store",5)){
                        if(e->nargs!=2) die(0,"buffer Store expects (value, byteOffset)");
                        ValKind vk; const char *vv=gen_rval(c,e->args[1],&vk);
                        int vw=c->rvw;
                        if(vw){
                            if(vw<1||vw>4) die(0,"buffer Store vector width %d unsupported",vw);
                            char *v32=newtmp(c);
                            emit(c,"  %s = bitcast i8 addrspace(%d)* %s to <4 x i32> addrspace(%d)*\n",v32,bp->ty.as,gp,bp->ty.as);
                            char *w=newtmp(c);
                            emit(c,"  %s = shufflevector <%d x i32> %s, <%d x i32> undef, <4 x i32> <i32 0, i32 1, i32 2, i32 3>\n",w,vw,vv,vw);
                            emit(c,"  store <4 x i32> %s, <4 x i32> addrspace(%d)* %s, align 4\n",w,bp->ty.as,v32);
                            c->written[ti]=1; *k=VK_U32; c->rvw=0; return vv;
                        }
                        char *v32=newtmp(c);
                        emit(c,"  %s = bitcast i8 addrspace(%d)* %s to i32 addrspace(%d)*\n",v32,bp->ty.as,gp,bp->ty.as);
                        emit(c,"  store i32 %s, i32 addrspace(%d)* %s, align 4\n",vv,bp->ty.as,v32);
                        c->written[ti]=1; *k=VK_U32; c->rvw=0; return vv;
                    }
                    char *v32=newtmp(c);
                    emit(c,"  %s = bitcast i8 addrspace(%d)* %s to i32 addrspace(%d)*\n",v32,bp->ty.as,gp,bp->ty.as);
                    const char *r=newtmp(c);
                    emit(c,"  %s = load i32, i32 addrspace(%d)* %s, align 4\n",r,bp->ty.as,v32);
                    *k=VK_U32; c->rvw=0; return r;
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
        /* matrix constructor mat2/3/4(...) / float2x2..float4x4(...) */
        {
            int mn=0;
            if(!strcmp(e->name,"mat2")) mn=2; else if(!strcmp(e->name,"mat3")) mn=3; else if(!strcmp(e->name,"mat4")) mn=4;
            else if(strlen(e->name)==8&&!strncmp(e->name,"float",5)&&e->name[6]=='x'&&!e->name[8]){
                int n1=e->name[5]-'0', n2=e->name[7]-'0';
                if(n1>=2&&n1<=4&&n2>=2&&n2<=4) mn=n1>n2?n1:n2; /* square narrowing target */
            }
            if(mn){
                if((int)e->nargs==1){
                    ValKind ak; const char *av=gen_rval(c,e->args[0],&ak);
                    if(c->rmat){ /* matrix argument: cast/narrow (float3x3(m) == (float3x3)m) */
                        if(c->rmat==mn){ *k=VK_F32; c->rvw=0; c->rmat=mn; return av; }
                        if(c->rmat<mn) die(0,"float%dx%d: cannot widen a matrix",mn,mn);
                        const char *res="undef";
                        for(int cc=0;cc<mn;cc++){
                            const char *colv="undef";
                            for(int rr=0;rr<mn;rr++){
                                const char *me=mat_elem(c,av,c->rmat,cc,rr);
                                const char *ins=newtmp(c);
                                emit(c,"  %s = insertelement <%d x float> %s, float %s, i32 %d\n",ins,mn,colv,me,rr);
                                colv=ins;
                            }
                            res=mat_setcol(c,res,mn,cc,colv);
                        }
                        *k=VK_F32; c->rvw=0; c->rmat=mn; return res;
                    }
                    /* diagonal matrix from one scalar */
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
                    for(int i=0;i<mn*mn;i++){ svals[i]=gen_rval(c,e->args[i],&aks[i]); if(c->rvw) die(0,"mat%d: scalar arguments must be scalars",mn);
                        if(aks[i]!=VK_F32){ svals[i]=coerce(c,svals[i],aks[i],VK_F32); aks[i]=VK_F32; } }
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
        /* scalar constructor/cast: float(x) / int(x) / uint(x) / bool(x) */
        if(!strcmp(e->name,"float")||!strcmp(e->name,"half")||!strcmp(e->name,"int")||
           !strcmp(e->name,"uint")||!strcmp(e->name,"bool")){
            if(e->nargs!=1) die(0,"%s expects 1 argument",e->name);
            ValKind ak; const char *v=gen_rval(c,e->args[0],&ak);
            if(c->rvw) die(0,"vector argument in %s constructor",e->name);
            TypeKind tk = !strcmp(e->name,"float")?T_FLOAT:!strcmp(e->name,"half")?T_HALF:
                          !strcmp(e->name,"int")?T_INT32:!strcmp(e->name,"uint")?T_UINT32:T_BOOL;
            *k=scalar_vk(tk); c->rvw=0;
            return coerce(c,v,ak,scalar_vk(tk));
        }
        /* vector constructor? float4(...)/int3(...)/uint2(...): 1 scalar (splat),
         * N scalars, or mixed scalars/vectors totalling N components (float3(v2, s), ...) */
        TypeKind cb; int cn;
        if(vec_name(e->name,&cb,&cn)){
            if(e->nargs!=1&&(int)e->nargs>cn) die(0,"%s expects 1 to %d argument(s)",e->name,cn);
            /* expression-level vectors are f32 (half demotes only at storage) */
            const char *elt=cb==T_HALF?"float":scalar_ll(cb); const char *acc="undef";
            if((int)e->nargs==1){
                ValKind ak; const char *v=gen_rval(c,e->args[0],&ak);
                if(c->rvw){
                    if(c->rvw!=cn) die(0,"vector width mismatch in %s constructor",e->name);
                    /* same-width vector constructor: element conversion (float3(i32v)) */
                    if(ak!=scalar_vk(cb)) v=vconv(c,v,cn,ak,scalar_vk(cb));
                    *k=scalar_vk(cb); c->rvw=cn;
                    return v;
                }
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
        /* user function? the whole Program is visible, so definition order doesn't matter.
         * HLSL overloads: rank-based selection (exact > int→float promotion >
         * scalar→vector splat); the best-ranked candidate wins. */
        Function *f=NULL; int best=1<<30;
        for(size_t i=0;i<c->prog->nfuncs;i++){
            Function *cf=&c->prog->funcs[i];
            if(getenv("BINC_DEBUG_IW")&&!strcmp(cf->name,e->name)){ fprintf(stderr,"DBG srch: %s line=%d nargs=%zu nparams=%zu kind=%d\n",e->name,e->line,e->nargs,cf->nparams,e->kind); if(e->nargs<=16){ for(size_t ai=0;ai<e->nargs;ai++) fprintf(stderr,"DBG   arg%zu: kind=%d%s%s\n",ai,e->args[ai]?e->args[ai]->kind:-1,e->args[ai]&&e->args[ai]->name?" name=":"",e->args[ai]&&e->args[ai]->name?e->args[ai]->name:""); } }
            if(strcmp(cf->name,e->name)) continue;
            if(e->nargs>cf->nparams) continue;
            int rk=0, bad=0;
            if(e->nargs<cf->nparams){ int no_def=0;
                for(size_t q=e->nargs;q<cf->nparams;q++) if(!cf->params[q].def){ no_def=1; break; }
                if(no_def) continue;
                rk+=3; /* uses defaulted args */ }
            for(size_t ai=0;ai<e->nargs;ai++){
                Type at={0}; expr_type_of(c,e->args[ai],&at);
                Type pt=cf->params[ai].ty;
                if(getenv("BINC_DEBUG_IW")&&!strcmp(cf->name,e->name)) fprintf(stderr,"DBG arg %zu: at(k=%d,ptr=%d,as=%d,v=%d) pt(k=%d,ptr=%d,as=%d,v=%d)\n",ai,at.kind,at.is_ptr,at.as,at.vecn,pt.kind,pt.is_ptr,pt.as,pt.vecn);
                if(pt.kind==T_TVAR) continue; /* template parameter matches anything */
                if(!strcmp(cf->params[ai].name,"__uniforms")) continue; /* __uniforms plumbing arg matches anything */
                if(pt.is_ptr){ if(!at.is_ptr||at.as!=pt.as){ bad=1; break; } continue; }
                if(at.kind!=pt.kind){
                    if(at.kind==T_INT32&&(pt.kind==T_FLOAT||pt.kind==T_HALF)&&!at.vecn&&!pt.vecn){ rk+=1; continue; } /* promotion */
                    if(at.kind==T_INT32&&pt.kind==T_FLOAT&&!at.vecn&&pt.vecn>0){ rk+=3; continue; } /* int scalar splat */
                    if(at.kind==T_FLOAT&&pt.kind==T_HALF&&!at.vecn&&!pt.vecn){ rk+=1; continue; } /* half demotion */
                    if((at.kind==T_UINT32&&pt.kind==T_INT32)||(at.kind==T_INT32&&pt.kind==T_UINT32)){ rk+=1; continue; } /* int/uint */
                    if((at.kind==T_UINT32||at.kind==T_INT32)&&pt.kind==T_BOOL&&!at.vecn&&!pt.vecn){ rk+=1; continue; } /* int/uint -> bool (CondMask(Flags & 0x100, ...)) */
                    bad=1; break;
                }
                if(at.kind==T_STRUCT&&at.struct_name&&pt.struct_name&&strcmp(at.struct_name,pt.struct_name)){ bad=1; break; } /* overloads on the struct tag (GetPrimitiveData(FMaterialVertexParameters) vs (FMaterialPixelParameters)) */
                if(at.vecn!=pt.vecn){
                    if(at.vecn==0&&pt.vecn>0){ rk+=2; continue; } /* scalar splat */
                    if(at.vecn>pt.vecn&&pt.vecn>0){ rk+=2; continue; } /* implicit vector truncation (float4 arg -> float3 param, legal HLSL; to_storage truncates at the call site) */
                    bad=1; break;
                }
            }
            if(!bad&&rk<best){ best=rk; f=cf; }
        }
        if(f){
            if(getenv("BINC_DEBUG_IW")&&!strcmp(e->name,"CondMask")&&!strcmp(c->fn->name,"GetPrimitive_PerObjectGBufferData_FromFlags")) fprintf(stderr,"DBG cm-found: f=%s np=%zu\n",f->name,f->nparams);
            if(f->is_template){
                /* instantiate for the concrete type of the first T-typed argument */
                size_t bp=(size_t)-1;
                for(size_t i=0;i<f->nparams;i++) if(f->params[i].ty.tvar){ bp=i; break; }
                if(bp==(size_t)-1) die(0,"cannot infer template arguments for %s (no type parameter in the parameters)",e->name);
                if(bp>=e->nargs) die(0,"%s: not enough arguments to infer the template type",e->name);
                Type concrete=expr_type(c,e->args[bp]);
                if(concrete.kind==T_TVAR||concrete.kind==T_VOID||concrete.kind==T_COORD||concrete.kind==T_GRID_EXTENT||
                   concrete.kind==T_ATOMIC||concrete.kind==T_TEXTURE||concrete.kind==T_SAMPLER||concrete.is_ptr)
                    die(0,"unsupported template argument type for %s",e->name);
                char key[128]; type_key(&concrete,key,sizeof key);
                char fullkey[192]; snprintf(fullkey,sizeof fullkey,"%s.%s",f->name,key);
                size_t fi=inst_lookup(fullkey);
                if(fi==(size_t)-1) fi=instantiate((Program*)c->prog,f,&concrete);
                f=&c->prog->funcs[fi];
                /* instantiate() reallocs prog->funcs; refresh the caller's context */
                c->fn=&c->prog->funcs[c->fidx];
            }
            if(f->is_kernel) die(0,"cannot call kernel function %s",e->name);
            if(f==c->fn) die(0,"recursion is not supported on the GPU (%s calls itself)",e->name);
            if(e->nargs>f->nparams) die(0,"%s expects at most %d argument(s), got %d",e->name,(int)f->nparams,(int)e->nargs);
            char args[2048]={0}; size_t o=0;
            if(getenv("BINC_DEBUG_IW")&&!strcmp(e->name,"CondMask")&&!strcmp(c->fn->name,"GetPrimitive_PerObjectGBufferData_FromFlags")){ fprintf(stderr,"DBG cm-loop: nparams=%zu",f->nparams); for(size_t qi=0;qi<f->nparams;qi++) fprintf(stderr," [%s k%d v%d p%d]",f->params[qi].name,f->params[qi].ty.kind,f->params[qi].ty.vecn,f->params[qi].ty.is_ptr); fprintf(stderr,"\n"); }
            for(size_t i=0;i<f->nparams;i++){ Param *p=&f->params[i];
                Expr *arge = (i<e->nargs)?e->args[i]:p->def; /* defaulted arg */
                if(!arge) die(0,"%s: missing argument %d (no default)",e->name,(int)i+1);
                if(i)o+=snprintf(args+o,sizeof args-o,", ");
                if(p->ty.kind==T_STRUCT && !p->ty.is_ptr){
                    /* struct by-value argument */
                    ValKind ak; const char *v=gen_rval(c,arge,&ak);
                    if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG structarg: %s arg%zu rstruct=%s want=%s rk=%d\n",e->name,i,c->rstruct?c->rstruct:"(null)",p->ty.struct_name?p->ty.struct_name:"(null)",arge->kind);
                    if(!c->rstruct||strcmp(c->rstruct,p->ty.struct_name))
                        die(0,"%s: struct type mismatch on argument %d",e->name,(int)i+1);
                    char sn[64]; snprintf(sn,sizeof sn,"%%struct.%s",p->ty.struct_name);
                    o+=snprintf(args+o,sizeof args-o,"%s %s",sn,v);
                } else if(p->ty.is_ptr){ Expr *a=arge; int pi;
                    if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG ptrarg: %s arg%zu name=%s fn=%s rk=%d\n",e->name,i,a->name?a->name:"(null)",c->fn->name,a->kind==E_IDENT?resolve(c,a->name,&pi):-99);
                    if(a->kind!=E_IDENT||resolve(c,a->name,&pi)!=R_PTR)
                        die(0,"%s: pointer argument %d must be one of the caller's buffer parameters",e->name,(int)i+1);
                    Param *cp=&c->fn->params[pi];
                    if(cp->ty.kind!=p->ty.kind||cp->ty.as!=p->ty.as||cp->ty.vecn!=p->ty.vecn||cp->ty.matn!=p->ty.matn)
                        die(0,"%s: pointer argument %d type/address-space mismatch",e->name,(int)i+1);
                    if(p->ty.kind==T_STRUCT && strcmp(cp->ty.struct_name,p->ty.struct_name))
                        die(0,"%s: pointer argument %d struct type mismatch",e->name,(int)i+1);
                    char elt[64];
                    if(p->ty.kind==T_STRUCT||p->ty.kind==T_ATOMIC) type_ll(elt,sizeof elt,p->ty.kind,p->ty.struct_name,p->ty.vecn);
                    else ll_of(elt,sizeof elt,p->ty.kind,p->ty.vecn);
                    o+=snprintf(args+o,sizeof args-o,"%s addrspace(%d)* %%_%s",elt,p->ty.as,cp->name);
                } else if(p->ty.matn){ /* matrix by-value argument */
                    ValKind ak; const char *v=gen_rval(c,arge,&ak);
                    if(!c->rmat) die(0,"%s: matrix argument %d is not a matrix",e->name,(int)i+1);
                    char mll[32]; mll_of(mll,sizeof mll,p->ty.matn,p->ty.matm);
                    o+=snprintf(args+o,sizeof args-o,"%s %s",mll,v);
                } else { ValKind ak; const char *v=gen_rval(c,arge,&ak);
                    if(getenv("BINC_DEBUG_IW")&&!strcmp(e->name,"CondMask")&&!strcmp(c->fn->name,"GetPrimitive_PerObjectGBufferData_FromFlags")){ fprintf(stderr,"DBG cm-arg%zu: kind=%d name=%s opk=%d\n",i,arge->kind,arge->name?arge->name:"",arge->operand?arge->operand->kind:-1);
                        if(arge->kind==E_FIELD&&arge->operand&&arge->operand->kind==E_IDENT) fprintf(stderr,"DBG cm-arg%zu-fld: base=%s field=%s\n",i,arge->operand->name,arge->field); }
                    warn_implicit(c,arge,ak,p->ty.kind,p->ty.vecn);
                    if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG callarg: %s arg%zu rvw=%d pt(k=%d,v=%d) line=%d\n",e->name,i,c->rvw,p->ty.kind,p->ty.vecn,e->line);
                    const char *sv=to_storage(c,v,ak,c->rvw,p->ty.kind,p->ty.vecn);
                    char ll[32]; ll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn);
                    o+=snprintf(args+o,sizeof args-o,"%s %s",ll,sv); }
            }
            if(f->ret.kind==T_VOID){ emit(c,"  call void @%s(%s)\n",f->link_name?f->link_name:f->name,args); *k=VK_I32; c->rvw=0; c->rmat=0; return "0"; }
            const char *r=newtmp(c);
            if(f->ret.matn){ /* matrix by-value return */
                if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG callret: %s matn=%d kind=%d sname=%s\n",e->name,f->ret.matn,f->ret.kind,f->ret.struct_name?f->ret.struct_name:"(null)");
                char mll[32]; mll_of(mll,sizeof mll,f->ret.matn,f->ret.matm);
                emit(c,"  %s = call %s @%s(%s)\n",r,mll,f->link_name?f->link_name:f->name,args);
                *k=VK_F32; c->rvw=0; c->rmat=f->ret.matn; return r;
            }
            if(f->ret.kind==T_STRUCT){
                char sn[64]; snprintf(sn,sizeof sn,"%%struct.%s",f->ret.struct_name);
                emit(c,"  %s = call %s @%s(%s)\n",r,sn,f->link_name?f->link_name:f->name,args);
                *k=VK_I32; c->rvw=0; c->rstruct=f->ret.struct_name; c->rmat=0; return r;
            }
            char rll[32]; ll_of(rll,sizeof rll,f->ret.kind,f->ret.vecn);
            emit(c,"  %s = call %s @%s(%s)\n",r,rll,f->link_name?f->link_name:f->name,args);
            if(f->ret.kind==T_HALF) r=h2f(c,r,f->ret.vecn);
            *k=scalar_vk(f->ret.kind); c->rvw=f->ret.vecn>1?f->ret.vecn:0; c->rmat=0; return r;
        }
        /* clip(x): discard the fragment if any component < 0 (HLSL clip
         * semantics) — lowers to air.discard_fragment behind a branch */
        if(!strcmp(e->name,"clip")){
            if(e->nargs!=1) die(0,"clip expects 1 argument");
            ValKind ak; const char *av=gen_rval(c,e->args[0],&ak); int aw=c->rvw;
            if(ak!=VK_F32) die(0,"clip argument must be a float");
            const char *cond;
            if(aw>1){
                char ty[32]; ll_of(ty,sizeof ty,T_FLOAT,aw);
                const char *cmp=newtmp(c);
                emit(c,"  %s = fcmp olt %s %s, zeroinitializer\n",cmp,ty,av);
                cond=newtmp(c);
                emit(c,"  %s = extractelement <%d x i1> %s, i32 0\n",cond,aw,cmp);
                for(int i=1;i<aw;i++){
                    const char *el=newtmp(c);
                    emit(c,"  %s = extractelement <%d x i1> %s, i32 %d\n",el,aw,cmp,i);
                    const char *o=newtmp(c);
                    emit(c,"  %s = or i1 %s, %s\n",o,cond,el);
                    cond=o;
                }
            } else {
                const char *z=fconst(c,0.0);
                cond=newtmp(c);
                emit(c,"  %s = fcmp olt float %s, %s\n",cond,av,z);
            }
            int disc=newlbl(c), cont=newlbl(c);
            emit(c,"  br i1 %s, label %%bb%d, label %%bb%d\n",cond,disc,cont);
            lbl(c,disc);
            emit(c,"  call void @air.discard_fragment()\n");
            g_uses_discard=1;
            emit(c,"  br label %%bb%d\n",cont);
            lbl(c,cont);
            *k=VK_I32; c->rvw=0; c->rmat=0; c->rstruct=NULL;
            return "0";
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
                for(int i=0;i<bi->nargs;i++) if(!wids[i]){ /* scalar arg splats to the vector width (mad(float3, float, float3)) */
                    const char *ael=aks[i]==VK_F32?"float":"i32";
                    vls[i]=splat(c,vls[i],ael,vw); }
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
            char args[256]={0}; size_t o=0;
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
    li->matn=0; li->an=0; li->am=0; /* every path must set vecn/matn/an/am explicitly; start clean */
    if(e->kind==E_IDENT){
        int idx; RKind r=resolve(c,e->name,&idx);
        if(r==R_LOCAL){ li->tk=c->locs[idx].kind; li->sname=c->locs[idx].sname; li->as=0; li->pi=-1;
            li->is_local=1; li->vecn=c->locs[idx].vecn; li->matn=c->locs[idx].matn;
            li->an=c->locs[idx].an; li->am=c->locs[idx].am;
            if(mark && c->locs[idx].is_const) die(0,"cannot write to const local %s",e->name);
            return c->locs[idx].slot; }
        if(r==R_CONST){ /* mutable static global (SPIRV-Cross pattern): a store
            to @_binc_mut_<name>; a write to a true const stays an error */
            ConstDef *cd=&c->prog->consts[idx];
            if(!cd->mut) die(0,"cannot write to const %s",e->name);
            li->tk=cd->ty.kind; li->sname=cd->ty.struct_name; li->as=1; li->pi=-1;
            li->is_local=0; li->vecn=cd->ty.vecn; li->matn=cd->ty.matn;
            li->an=cd->ty.array_n; li->am=cd->ty.array_m;
            char gn[160]; snprintf(gn,sizeof gn,"@_binc_mut_%s",cd->name);
            return strdup(gn); }
        if(r==R_SCALAR){ /* plain scalar/vector parameter as an lvalue (e.g. the
            base of a scalar swizzle like `bool a` with a.x, or a by-value
            param being written like `uv += ...`): the by-value param needs an
            alloca to take its address; writes mutate the local copy */
            Param *pr=&c->fn->params[idx];
            fill_param_li(c,idx,li); li->an=pr->ty.array_n; li->am=pr->ty.array_m;
            char pty[96];
            if(li->matn) mll_of(pty,sizeof pty,li->matn,0);
            else type_ll(pty,sizeof pty,li->tk,li->sname,li->vecn);
            char *as=newtmp(c);
            emit(c,"  %s = alloca %s\n",as,pty);
            emit(c,"  store %s %%_%s, %s* %s\n",pty,e->name,pty,as);
            return as; }
        die(0, mark ? "%s is not a mutable local" : "undefined name %s", e->name);
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
            char mty[32]; mll_of(mty,sizeof mty,mn,0);
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
                char mty[32]; mll_of(mty,sizeof mty,mn2,0);
                char *colptr=newtmp(c);
                emit(c,"  %s = getelementptr inbounds %s, %s* %s, i64 0, i64 %s\n",colptr,mty,mty,base,iv);
                li->tk=T_FLOAT; li->sname=NULL; li->vecn=mn2; li->matn=0;
                return colptr;
            }
        }
        /* local and struct-field arrays: a[i], s.v[i], buf[i].v[j] */
        {
            int is_arr = (e->operand->kind==E_FIELD||e->operand->kind==E_INDEX);
            if(e->operand->kind==E_IDENT){ int oi; is_arr = resolve(c,e->operand->name,&oi)==R_LOCAL; }
            if(is_arr){
                LInfo li2; char *base2=gen_lval(c,e->operand,&li2,mark);
                if(li2.an>0){
                    ValKind ik; const char *iv=gen_rval(c,e->rhs,&ik);
                    if(ik!=VK_I32&&ik!=VK_U32) die(0,"array index must be an integer");
                    const char *ix=newtmp(c);
                    emit(c,"  %s = sext i32 %s to i64\n",ix,iv);
                    char elt[32]; ll_of(elt,sizeof elt,li2.tk,li2.vecn);
                    char asp[32]; if(li2.as) snprintf(asp,sizeof asp," addrspace(%d)",li2.as); else asp[0]='\0';
                    if(li2.am>0){
                        /* two-dimensional: [N x [M x T]]* -> row [M x T]* */
                        char ty[64]; snprintf(ty,sizeof ty,"[%d x [%d x %s]]",li2.an,li2.am,elt);
                        char *p=newtmp(c);
                        emit(c,"  %s = getelementptr inbounds %s, %s%s* %s, i64 0, i64 %s\n",p,ty,ty,asp,base2,ix);
                        li->tk=li2.tk; li->sname=li2.sname; li->as=li2.as; li->pi=li2.pi; li->is_local=li2.is_local;
                        li->vecn=li2.vecn; li->matn=0; li->an=li2.am; li->am=0;
                        return p;
                    }
                    char ty[64]; snprintf(ty,sizeof ty,"[%d x %s]",li2.an,elt);
                    char *p=newtmp(c);
                    emit(c,"  %s = getelementptr inbounds %s, %s%s* %s, i64 0, i64 %s\n",p,ty,ty,asp,base2,ix);
                    li->tk=li2.tk; li->sname=li2.sname; li->as=li2.as; li->pi=li2.pi; li->is_local=li2.is_local;
                    li->vecn=li2.vecn; li->matn=0; li->an=0; li->am=0;
                    if(li2.pi>=0){ if(mark) c->written[li2.pi]=1; else c->read[li2.pi]=1; }
                    return p;
                }
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
        else { if(getenv("BINC_DEBUG_D3D9")) fprintf(stderr,"DBG sub: operand-kind=%d field=%s line=%d\n",e->operand->kind,e->operand->field?e->operand->field:"",e->line); pi=eval_ptr(c,e->operand,&base); } /* e.g. (p+1)[i] */
        ValKind ik; const char *iv=gen_rval(c,e->rhs,&ik);
        if(ik!=VK_I32&&ik!=VK_U32) die(0,"subscript must be an integer");
        const char *i64=newtmp(c); emit(c,"  %s = sext i32 %s to i64\n",i64,iv);
        if(base){ char *s=newtmp(c); emit(c,"  %s = add i64 %s, %s\n",s,base,i64); i64=s; }
        if(mark)c->written[pi]=1; fill_param_li(c,pi,li); return element_ptr_idx(c,pi,i64);
    }
    if(e->kind==E_FIELD){
        /* struct-pointer base: ptr.field (cbuffers, device struct buffers) */
        if(e->operand->kind==E_IDENT){
            int pi; if(resolve(c,e->operand->name,&pi)==R_PTR){
                Param *pp=&c->fn->params[pi];
                if(pp->ty.kind==T_STRUCT){
                    StructDef *s=find_struct(c->prog,pp->ty.struct_name); if(!s) die(0,"unknown struct %s",pp->ty.struct_name);
                    int fi=-1; for(size_t i=0;i<s->nfields;i++) if(!strcmp(s->fields[i].name,e->field)) fi=(int)i;
                    if(fi<0) die(0,"struct %s has no field %s",pp->ty.struct_name,e->field);
                    char *fp=newtmp(c);
                    emit(c,"  %s = getelementptr inbounds %%struct.%s, %%struct.%s addrspace(%d)* %%_%s, i32 0, i32 %d\n",fp,pp->ty.struct_name,pp->ty.struct_name,pp->ty.as,pp->name,fi);
                    if(mark)c->written[pi]=1; else c->read[pi]=1;
                    li->tk=s->fields[fi].ty.kind; li->sname=s->fields[fi].ty.struct_name;
                    li->vecn=s->fields[fi].ty.vecn; li->matn=s->fields[fi].ty.matn;
                    li->an=s->fields[fi].ty.array_n; li->am=s->fields[fi].ty.array_m;
                    li->as=pp->ty.as; li->pi=pi; li->is_local=0;
                    return fp;
                }
            }
        }
        char *addr=gen_lval(c,e->operand,li,mark);
        if(li->an>0&&li->vecn==0&&li->matn==0&&li->tk==T_FLOAT){
            /* packed-vector component: b.x on a [N x float] cbuffer field */
            int ci=comp_idx(e->field);
            if(ci<0||ci>=li->an) die(0,"no component .%s on the packed vector",e->field);
            char pty[96]; snprintf(pty,sizeof pty,"[%d x float]",li->an);
            char *cp=newtmp(c);
            emit(c,"  %s = getelementptr inbounds %s, %s addrspace(%d)* %s, i64 0, i64 %d\n",cp,pty,pty,li->as,addr,ci);
            li->an=0;
            return cp;
        }
        if(li->matn>0&&e->field[0]=='_'&&(e->field[1]=='m'||(e->field[1]>='1'&&e->field[1]<='4'))){
            /* matrix element lvalue: M._m10 (0-based) or M._44 (1-based shorthand) */
            int r=-1, cc=-1;
            if(e->field[1]=='m'&&e->field[2]>='0'&&e->field[2]<='4'&&e->field[3]>='0'&&e->field[3]<='4'){ r=e->field[2]-'0'; cc=e->field[3]-'0'; }
            else if(e->field[1]>='1'&&e->field[1]<='4'&&e->field[2]>='1'&&e->field[2]<='4'){ r=e->field[1]-'1'; cc=e->field[2]-'1'; }
            if(r<0||r>=li->matn||cc<0||cc>=li->matn) die(0,"no element .%s on mat%d",e->field,li->matn);
            char mty[32]; mll_of(mty,sizeof mty,li->matn,0);
            char mpty[64]; if(li->as) snprintf(mpty,sizeof mpty,"%s addrspace(%d)*",mty,li->as); else snprintf(mpty,sizeof mpty,"%s*",mty);
            char *cp=newtmp(c);
            emit(c,"  %s = getelementptr inbounds %s, %s %s, i64 0, i64 %d\n",cp,mty,mpty,addr,cc);
            char *ep=newtmp(c);
            if(li->as) emit(c,"  %s = getelementptr inbounds <%d x float>, <%d x float> addrspace(%d)* %s, i64 0, i64 %d\n",ep,li->matn,li->matn,li->as,cp,r);
            else emit(c,"  %s = getelementptr inbounds <%d x float>, <%d x float>* %s, i64 0, i64 %d\n",ep,li->matn,li->matn,cp,r);
            li->tk=T_FLOAT; li->sname=NULL; li->vecn=0; li->matn=0;
            return ep;
        }
        if(li->tk==T_STRUCT){
            StructDef *s=find_struct(c->prog,li->sname); if(!s) die(0,"unknown struct %s",li->sname);
            int fi=-1; for(size_t i=0;i<s->nfields;i++) if(!strcmp(s->fields[i].name,e->field)) fi=(int)i;
            if(fi<0) die(0,"struct %s has no field %s",li->sname,e->field);
            char pty[96]; pty_str(pty,sizeof pty,T_STRUCT,li->sname,li->as,li->is_local,0,0);
            char *fp=newtmp(c);
            emit(c,"  %s = getelementptr inbounds %%struct.%s, %s %s, i64 0, i32 %d\n",fp,li->sname,pty,addr,fi);
            li->tk=s->fields[fi].ty.kind; li->sname=s->fields[fi].ty.struct_name; li->vecn=s->fields[fi].ty.vecn; li->matn=s->fields[fi].ty.matn;
            li->an=s->fields[fi].ty.array_n; li->am=s->fields[fi].ty.array_m;
            return fp; }
        if(li->vecn>1||(li->an>0&&li->an<=4&&!li->am)){ /* vector component (incl. packed [3 x float] fields): bitcast the vector address to element pointer + gep */
            int fv=li->vecn>1?li->vecn:li->an;
            int ci=comp_idx(e->field);
            if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG gllval: field=%s ci=%d vecn=%d line=%d\n",e->field,ci,fv,e->line);
            if(ci<0||ci>=fv) die(0,"no component .%s on <%d x %s>",e->field,fv,scalar_ll(li->tk));
            char vpty[96],epty[96];
            pty_str(vpty,sizeof vpty,li->tk,NULL,li->as,li->is_local,fv,li->matn);
            pty_str(epty,sizeof epty,li->tk,NULL,li->as,li->is_local,0,0);
            char *bc=newtmp(c); emit(c,"  %s = bitcast %s %s to %s\n",bc,vpty,addr,epty);
            char *cp=newtmp(c);
            emit(c,"  %s = getelementptr inbounds %s, %s %s, i64 %d\n",cp,scalar_ll(li->tk),epty,bc,ci);
            li->vecn=0; return cp; }
        if(li->vecn==0&&li->matn==0&&comp_idx(e->field)==0){ /* scalar swizzle: `a.x` on a scalar is a no-op */
            return addr; }
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
/* divergence heuristic: does expr touch per-thread (device-buffer / coordinate)
 * data (=> varying)? constant-buffer reads are uniform across a threadgroup */
static int is_varying(CG *c, Expr *e){
    if(!e) return 0;
    if(e->kind==E_IDENT){ int i; RKind r=resolve(c,e->name,&i);
        if(r==R_COORD) return 1;
        if(r==R_SCALAR && c->fn->params[i].un==UN_VARYING) return 1;
    }
    if(e->kind==E_DEREF||e->kind==E_FIELD||e->kind==E_INDEX){
        int pi=root_param(c,e);
        if(pi>=0 && c->fn->params[pi].ty.as!=AS_CONSTANT) return 1; /* constant = uniform */
    }
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
    case S_DECL:{ Type bty=bind_ty(c->fn,s->ty); TypeKind kk=bty.kind;
        if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG decl: fn=%s name=%s kind=%d initk=%d\n",c->fn->name,s->name,kk,s->init?s->init->kind:-1);
        if(getenv("BINC_DEBUG_IW")&&!strcmp(c->fn->name,"GetPrimitive_PerObjectGBufferData_FromFlags")) fprintf(stderr,"DBG decl: %s kind=%d initk=%d\n",s->name,kk,s->init?s->init->kind:-1);
        if(kk==T_STRUCT){ StructDef *sd=find_struct(c->prog,bty.struct_name);
            if(!sd) die(0,"unknown struct %s",bty.struct_name);
            if(bty.array_n) die(0,"arrays of structs are not supported as locals");
            int sal; struct_layout(sd,&sal);
            char *slot=newtmp(c); sb_printf(c->pre,"  %s = alloca %%struct.%s, align %d\n",slot,bty.struct_name,sal);
            c->locs=realloc(c->locs,(c->nlocs+1)*sizeof(Loc));
            c->locs[c->nlocs++]=(Loc){s->name,slot,kk,bty.struct_name,0,0,0,0,0};
            if(s->init){ ValKind k; const char *v=gen_rval(c,s->init,&k);
                if(!c->rstruct||strcmp(c->rstruct,bty.struct_name)) die(0,"struct type mismatch in initializer");
                char sn[64]; snprintf(sn,sizeof sn,"%%struct.%s",bty.struct_name);
                emit(c,"  store %s %s, %s* %s, align %d\n",sn,v,sn,slot,sal); }
            break; }
        if(bty.matn){
            char mty[32]; mll_of(mty,sizeof mty,bty.matn,bty.matm);
            char *slot=newtmp(c); sb_printf(c->pre,"  %s = alloca %s, align %d\n",slot,mty,mat_align(bty.matn));
            c->locs=realloc(c->locs,(c->nlocs+1)*sizeof(Loc));
            c->locs[c->nlocs++]=(Loc){s->name,slot,kk,NULL,0,bty.matn,s->is_const,0,0};
            if(s->init){ ValKind k; const char *v=gen_rval(c,s->init,&k);
                if(c->rmat!=bty.matn) die(0,"matrix width mismatch in initializer");
                emit(c,"  store %s %s, %s* %s, align %d\n",mty,v,mty,slot,mat_align(bty.matn)); }
            break; }
        if(bty.array_n){
            /* local fixed-size array: alloca [N x T] / [N x [M x T]] */
            char elt[32]; ll_of(elt,sizeof elt,kk,bty.vecn);
            char arrty[64];
            if(bty.array_m) snprintf(arrty,sizeof arrty,"[%d x [%d x %s]]",bty.array_n,bty.array_m,elt);
            else snprintf(arrty,sizeof arrty,"[%d x %s]",bty.array_n,elt);
            if(s->init) die(0,"array initializers are not supported; assign elements instead");
            char *slot=newtmp(c); sb_printf(c->pre,"  %s = alloca %s, align %d\n",slot,arrty,type_align(kk,bty.vecn));
            c->locs=realloc(c->locs,(c->nlocs+1)*sizeof(Loc));
            c->locs[c->nlocs++]=(Loc){s->name,slot,kk,NULL,bty.vecn,0,s->is_const,bty.array_n,bty.array_m};
            break; }
        char ll[32]; ll_of(ll,sizeof ll,kk,bty.vecn);
        char *slot=newtmp(c); sb_printf(c->pre,"  %s = alloca %s, align %d\n",slot,ll,type_align(kk,bty.vecn));
        c->locs=realloc(c->locs,(c->nlocs+1)*sizeof(Loc));
        c->locs[c->nlocs++]=(Loc){s->name,slot,kk,NULL,bty.vecn,0,s->is_const,0,0};
        if(s->init){ ValKind k; const char *v=gen_rval(c,s->init,&k);
            if(getenv("BINC_DEBUG_IW")&&!strcmp(c->fn->name,"GetPrimitive_PerObjectGBufferData_FromFlags")) fprintf(stderr,"DBG decl-init: kind=%d name=%s nargs=%zu rvw=%d rstruct=%s\n",s->init->kind,s->init->name?s->init->name:"",s->init->nargs,c->rvw,c->rstruct?c->rstruct:"(null)");
            warn_implicit(c,s->init,k,kk,bty.vecn);
            const char *sv=to_storage(c,v,k,c->rvw,kk,bty.vecn);
            emit(c,"  store %s %s, %s* %s, align %d\n",ll,sv,ll,slot,type_align(kk,bty.vecn)); }
        break; }
    case S_RETURN:{ TypeKind rk=c->fn->ret.kind;
        if(s->expr){ if(rk==T_VOID) die(0,"return with a value in void function");
            if(rk==T_STRUCT){
                /* stage struct return: build the literal output struct from a struct local */
                if(c->fn->stage==ST_NONE){
                    /* plain function: return the struct value directly */
                    ValKind k; const char *v=gen_rval(c,s->expr,&k);
                    if(!c->rstruct||strcmp(c->rstruct,c->fn->ret.struct_name)) die(0,"struct type mismatch in return");
                    char sn[64]; snprintf(sn,sizeof sn,"%%struct.%s",c->fn->ret.struct_name);
                    emit(c,"  ret %s %s\n",sn,v);
                } else {
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
            } } else if(c->fn->ret.matn){
                ValKind k; const char *v=gen_rval(c,s->expr,&k);
                if(c->rmat!=c->fn->ret.matn) die(0,"matrix width mismatch in return");
                char mll[32]; mll_of(mll,sizeof mll,c->fn->ret.matn,c->fn->ret.matm);
                emit(c,"  ret %s %s\n",mll,v);
            } else {
                ValKind k; const char *v=gen_rval(c,s->expr,&k);
                if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG stor-R: rk=%d rvw=%d retvecn=%d line=%d\n",k,c->rvw,c->fn->ret.vecn,s->expr->line);
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
        else if(p->ty.is_ptr){ char elt[64]; if(p->ty.matn) mll_of(elt,sizeof elt,p->ty.matn,p->ty.matm); else type_ll(elt,sizeof elt,p->ty.kind,p->ty.struct_name,p->ty.vecn);
            o+=snprintf(buf+o,n-o,"%s addrspace(%d)*",elt,p->ty.as); }
        else if(fn->is_kernel){ char ll[32]; ll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn); o+=snprintf(buf+o,n-o,"%s addrspace(2)*",ll); }
        else { char ll[32]; ll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn); o+=snprintf(buf+o,n-o,"%s",ll); }
    }
    if(fn->is_kernel){ if(ex){ Param *cp=NULL; for(size_t i=0;i<fn->nparams;i++)if(fn->params[i].ty.kind==T_COORD){cp=&fn->params[i];break;} char cl[32]; coord_ll(&cp->ty,cl,sizeof cl);
            if(emitted++)o+=snprintf(buf+o,n-o,", "); o+=snprintf(buf+o,n-o,"%s, %s",cl,cl);
        } else { if(emitted++)o+=snprintf(buf+o,n-o,", "); o+=snprintf(buf+o,n-o,"i32"); } }
    o+=snprintf(buf+o,n-o,")* @%s",fn->link_name?fn->link_name:fn->name);
}
/* AIR contract of the installed toolchain; set by binc_set_air() (main.c detection) */
char g_air_triple[64]="air64_v29-apple-macosx27.0.0";
int g_air_sdk=27, g_air_minor=9;
int g_uses_discard=0;
void binc_set_air(const char *triple, int sdk, int minor){
    snprintf(g_air_triple,sizeof g_air_triple,"%s",triple); g_air_sdk=sdk; g_air_minor=minor; }
/* a struct type with no fields (UE stubs like FStereoVSToPS): contributes no
 * stage-in args / metadata nodes / copies */
static int struct_empty(const Program *prog, const char *tag){
    if(!tag) return 0;
    for(size_t i=0;i<prog->nstructs;i++)
        if(!strcmp(prog->structs[i].tag,tag)) return prog->structs[i].nfields==0;
    return 0;
}
static void stage_lit_type(char *buf, size_t n, StructDef *sd);
static void stage_ptr_str(Function *fn,char *buf,size_t n){
    size_t o=0;
    if(fn->ret.kind==T_STRUCT){ StructDef *sd=find_struct(g_curprog,fn->ret.struct_name);
        char lt[256]; stage_lit_type(lt,sizeof lt,sd); o+=snprintf(buf+o,n-o,"%s (",lt); }
    else { char rl[32]; ll_of(rl,sizeof rl,fn->ret.kind,fn->ret.vecn); o+=snprintf(buf+o,n-o,"%s (",rl); }
    int emitted=0;
    for(size_t i=0;i<fn->nparams;i++){ Param *p=&fn->params[i];
        if(p->ty.kind==T_STRUCT && !p->ty.is_ptr && fn->stage!=ST_NONE){
            /* stage-in struct (vertex or fragment): unpacked as separate args */
            StructDef *sd=find_struct(g_curprog,p->ty.struct_name);
            for(size_t f=0;f<sd->nfields;f++){ if(struct_empty(g_curprog,sd->fields[f].ty.struct_name)) continue;
                char fl[64]; type_ll(fl,sizeof fl,sd->fields[f].ty.kind,sd->fields[f].ty.struct_name,sd->fields[f].ty.vecn);
                if(emitted++)o+=snprintf(buf+o,n-o,", "); o+=snprintf(buf+o,n-o,"%s",fl); }
            continue;
        }
        char pl[64]; type_ll(pl,sizeof pl,p->ty.kind,p->ty.struct_name,p->ty.vecn);
        if(emitted++)o+=snprintf(buf+o,n-o,", ");
        if(p->ty.kind==T_TEXTURE) o+=snprintf(buf+o,n-o,"%%struct._texture_2d_t addrspace(1)*");
        else if(p->ty.kind==T_SAMPLER) o+=snprintf(buf+o,n-o,"%%struct._sampler_t addrspace(2)*");
        else if(p->ty.is_ptr) o+=snprintf(buf+o,n-o,"%s addrspace(%d)*",pl,p->ty.as);
        else o+=snprintf(buf+o,n-o,"%s",pl);
    }
    o+=snprintf(buf+o,n-o,")* @%s",fn->link_name?fn->link_name:fn->name);
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

/* element size/align of a (possibly struct-typed) buffer element; structs use
 * the real padded layout (StructuredBuffer<MyStruct> stride metadata) */
static int elem_size(const Program *prog, Type *ty){ if(ty->kind==T_STRUCT&&ty->struct_name){ StructDef *s=find_struct(prog,ty->struct_name); if(s){ int al; return struct_layout(s,&al); } } return tsz(ty->kind,ty->vecn,ty->matn); }
static int elem_align(const Program *prog, Type *ty){ if(ty->kind==T_STRUCT&&ty->struct_name){ StructDef *s=find_struct(prog,ty->struct_name); if(s){ int al; struct_layout(s,&al); return al; } } return tal(ty->kind,ty->vecn,ty->matn); }

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
    int argidx=0; /* the actual argument index: threadgroup arrays take no args */
    for(int a=0;a<(int)fn->nparams;a++){ Param *p=&fn->params[a]; if(p->ty.array_n)continue;
        if(p->ty.kind==T_COORD){ char cn[32]; snprintf(cn,sizeof cn,p->ty.coordn==1?"uint":p->ty.coordn==2?"ushort2":"ushort3");
            meta_emit(m,"!%d = !{i32 %d, !\"air.thread_position_in_grid\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",km->argnode[ai++],argidx,cn,p->name); argidx++; continue; }
        if(p->ty.kind==T_GRID_EXTENT){ meta_emit(m,"!%d = !{i32 %d, !\"air.threads_per_grid\", !\"air.arg_type_name\", !\"uint\", !\"air.arg_name\", !\"%s\"}\n",km->argnode[ai++],argidx,p->name); argidx++; continue; }
        if(p->ty.kind==T_TEXTURE){ const char *elt,*vec,*suf,*an; tex_kinds(p->ty.tex_elt,&elt,&vec,&suf,&an);
            meta_emit(m,"!%d = !{i32 %d, !\"air.texture\", !\"air.location_index\", i32 %d, i32 1, !\"air.read_write\", !\"air.arg_type_name\", !\"texture2d<%s, read_write>\", !\"air.arg_name\", !\"%s\"}\n",km->argnode[ai++],argidx,argidx,elt,p->name); argidx++; continue; }
        if(p->ty.kind==T_SAMPLER){
            meta_emit(m,"!%d = !{i32 %d, !\"air.sampler\", !\"air.location_index\", i32 %d, i32 1, !\"air.arg_type_name\", !\"sampler\", !\"air.arg_name\", !\"%s\"}\n",km->argnode[ai++],argidx,argidx,p->name); argidx++; continue; }
        if(p->ty.is_ptr){ int r=read[a],w=written[a]; const char *acc=(r&&w)?"air.read_write":w?"air.write":"air.read";
            int sz=tsz(p->ty.kind,p->ty.vecn,p->ty.matn),al=tal(p->ty.kind,p->ty.vecn,p->ty.matn); char tnb[64];
            if(getenv("BINC_DEBUG_IW")&&!strcmp(p->name,"__uniforms")) fprintf(stderr,"DBG meta-u: kind=%d sname=%s is_ptr=%d as=%d vecn=%d\n",p->ty.kind,p->ty.struct_name?p->ty.struct_name:"(null)",p->ty.is_ptr,p->ty.as,p->ty.vecn);
            ptn_of(tnb,sizeof tnb,p->ty.kind,p->ty.vecn,p->ty.matn); const char *tn=tnb;
            if(p->ty.kind==T_ATOMIC){ snprintf(tnb,sizeof tnb,"metal::_atomic"); tn=tnb; }
            if(p->ty.kind==T_STRUCT){ StructDef *s=find_struct(prog,p->ty.struct_name);
                if(s){ sz=struct_layout(s,&al); snprintf(tnb,sizeof tnb,"%s",p->ty.struct_name); tn=tnb; }
                else { sz=4; al=4; snprintf(tnb,sizeof tnb,"%s",p->ty.struct_name); tn=tnb; } }
            int an=km->argnode[ai++];
            if(p->ty.kind==T_ATOMIC) meta_emit(m,"!%d = !{i32 %d, !\"air.buffer\", !\"air.location_index\", i32 %d, i32 1, !\"%s\", !\"air.address_space\", i32 %d, !\"air.struct_type_info\", !%d, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",an,argidx,argidx,acc,p->ty.as,km->structnode[a],sz,al,tn,p->name);
            else meta_emit(m,"!%d = !{i32 %d, !\"air.buffer\", !\"air.location_index\", i32 %d, i32 1, !\"%s\", !\"air.address_space\", i32 %d, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",an,argidx,argidx,acc,p->ty.as,sz,al,tn,p->name); argidx++; }
        else { char tnb[64]; ptn_of(tnb,sizeof tnb,p->ty.kind,p->ty.vecn,p->ty.matn);
            meta_emit(m,"!%d = !{i32 %d, !\"air.buffer\", !\"air.buffer_size\", i32 %d, !\"air.location_index\", i32 %d, i32 1, !\"air.read\", !\"air.address_space\", i32 2, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",km->argnode[ai++],argidx,elem_size(prog,&p->ty),argidx,elem_size(prog,&p->ty),elem_align(prog,&p->ty),tnb,p->name); argidx++; } }
    for(int a=0;a<np;a++) if(km->structnode[a]>=0){ Param *p=&fn->params[a];
        meta_emit(m,"!%d = !{i32 0, i32 4, i32 0, !\"%s\", !\"__s\"}\n",km->structnode[a],type_name(p->ty.atomic_base)); }
    if(cp){ char cn[32]; snprintf(cn,sizeof cn,cp->ty.coordn==1?"uint":cp->ty.coordn==2?"ushort2":"ushort3");
        meta_emit(m,"!%d = !{i32 %d, !\"air.thread_position_in_threadgroup\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s_local\"}\n",km->argnode[ai++],argidx,cn,cp->name);
        meta_emit(m,"!%d = !{i32 %d, !\"air.threadgroup_position_in_grid\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s_group\", !\"air.arg_unused\"}\n",km->argnode[ai++],argidx+1,cn,cp->name);
    } else {
        meta_emit(m,"!%d = !{i32 %d, !\"air.thread_position_in_grid\", !\"air.arg_type_name\", !\"uint\", !\"air.arg_name\", !\"id\"}\n",km->argnode[ai++],argidx);
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
    meta_emit(m,"!%d = !{%s, !%d, !%d}\n",sm->node,fp,sm->outs,sm->args);
    int argi=0; int locn=0; /* cumulative across stage-in structs: each unpacked field gets a unique user(locnN) */
    for(size_t a=0;a<fn->nparams;a++){ Param *p=&fn->params[a]; char tn[64]; ptn_of(tn,sizeof tn,p->ty.kind,p->ty.vecn,p->ty.matn);
        if(p->ty.kind==T_STRUCT&&p->ty.struct_name) snprintf(tn,sizeof tn,"%s",p->ty.struct_name); /* buffer structs: the AIR frontend needs the struct tag (kernel emitter parity) */
        if(fn->stage!=ST_NONE && p->ty.kind==T_STRUCT && !p->ty.is_ptr){
            /* stage-in struct: one metadata node per unpacked field */
            StructDef *sd=find_struct(prog,p->ty.struct_name);
            for(size_t f=0;f<sd->nfields;f++){ if(struct_empty(prog,sd->fields[f].ty.struct_name)) continue;
                Field *fd=&sd->fields[f]; char ftn[64]; ptn_of(ftn,sizeof ftn,fd->ty.kind,fd->ty.vecn,0);
                if(fn->stage==ST_VERTEX){
                    int ln=fd->attr==5?fd->attr_idx:locn; locn++;
                    /* probed from real metal output: air.vertex_input +
                     * air.location_index, then a trailing i32 1 (buffer flag) */
                    meta_emit(m,"!%d = !{i32 %d, !\"air.vertex_input\", !\"air.location_index\", i32 %d, i32 1, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,ln,ftn,fd->name);
                } else if(fd->attr==1){
                    meta_emit(m,"!%d = !{i32 %d, !\"air.position\", !\"air.center\", !\"air.no_perspective\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,ftn,fd->name);
                } else if(fd->sem && strncasecmp(fd->sem,"SV_IsFrontFace",14)==0){
                    /* SV_IsFrontFace -> the front_facing attribute (no user(locnN)) */
                    meta_emit(m,"!%d = !{i32 %d, !\"air.front_facing\", !\"air.arg_type_name\", !\"bool\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,fd->name);
                } else { int ln=fd->attr==5?fd->attr_idx:locn; locn++;
                    meta_emit(m,"!%d = !{i32 %d, !\"air.fragment_input\", !\"user(locn%d)\", !\"air.center\", !\"%s\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,ln,fd->attr==2?"air.flat":"air.perspective",ftn,fd->name); }
                argi++;
            }
            continue;
        }
        if(p->ty.kind==T_TEXTURE){ /* probed: air.texture with access flags */
            meta_emit(m,"!%d = !{i32 %d, !\"air.texture\", !\"air.location_index\", i32 %d, i32 1, !\"air.sample\", !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,argi,p->ty.tex_cube?"texturecube<float, sample>":"texture2d<float, sample>",p->name); }
        else if(p->ty.kind==T_SAMPLER){ /* probed: air.sampler */
            meta_emit(m,"!%d = !{i32 %d, !\"air.sampler\", !\"air.location_index\", i32 %d, i32 1, !\"air.arg_type_name\", !\"sampler\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,argi,p->name); }
        else if(fn->stage==ST_VERTEX && p->ty.as==AS_THREAD){ meta_emit(m,"!%d = !{i32 %d, !\"air.vertex_id\", !\"air.arg_type_name\", !\"uint\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,p->name); }
        else if(fn->stage==ST_FRAGMENT && p->ty.kind==T_FLOAT && p->ty.vecn==4 && p->ty.as==AS_THREAD){ meta_emit(m,"!%d = !{i32 %d, !\"air.position\", !\"air.center\", !\"air.no_perspective\", !\"air.arg_type_name\", !\"float4\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,p->name); }
        else if(p->ty.is_ptr) meta_emit(m,"!%d = !{i32 %d, !\"air.buffer\", !\"air.location_index\", i32 %d, i32 1, !\"air.read\", !\"air.address_space\", i32 %d, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,argi,p->ty.as,elem_size(prog,&p->ty),elem_align(prog,&p->ty),tn,p->name);
        else meta_emit(m,"!%d = !{i32 %d, !\"air.buffer\", !\"air.buffer_size\", i32 %d, !\"air.location_index\", i32 %d, i32 1, !\"air.read\", !\"air.address_space\", i32 2, !\"air.arg_type_size\", i32 %d, !\"air.arg_type_align_size\", i32 %d, !\"air.arg_type_name\", !\"%s\", !\"air.arg_name\", !\"%s\"}\n",sm->argnode[argi],argi,elem_size(prog,&p->ty),argi,elem_size(prog,&p->ty),elem_align(prog,&p->ty),tn,p->name);
        argi++;
    }
    /* the arg list must follow the nodes: empty-struct fields are skipped, so
     * only the actually-emitted nodes (first argi) get listed */
    meta_emit(m,"!%d = !{",sm->args); for(int a=0;a<argi;a++){ if(a)meta_emit(m,", "); meta_emit(m,"!%d",sm->argnode[a]); } meta_emit(m,"}\n");
}

void emit_air(FILE *out, Program *prog){
    g_curprog=prog;
    memset(builtin_used,0,sizeof builtin_used); memset(atomic_add_used,0,sizeof atomic_add_used);
    fprintf(out,"; generated by binc — works as C, acts as Metal\n");
    fprintf(out,"target datalayout = \"e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32\"\n");
    fprintf(out,"target triple = \"%s\"\n\n",g_air_triple);
    /* module-level constant globals: scalar numeric values in address space 2;
     * mutable static globals (SPIRV-Cross pattern) in address space 1 */
    for(size_t i=0;i<prog->nconsts;i++){ ConstDef *cd=&prog->consts[i];
        if(cd->ty.kind==T_STRUCT){ /* struct type from a missing generated include: skip */
            int known=0;
            for(size_t j=0;j<prog->nstructs;j++) if(!strcmp(prog->structs[j].tag,cd->ty.struct_name)){ known=1; break; }
            if(!known) continue;
        }
        char ll[64];
        if(cd->ty.kind==T_STRUCT) snprintf(ll,sizeof ll,"%%struct.%s",cd->ty.struct_name);
        else ll_of(ll,sizeof ll,cd->ty.kind,cd->mut?cd->ty.vecn:0);
        char val[64];
        if(cd->mut) snprintf(val,sizeof val,"zeroinitializer");
        else if(cd->ty.kind==T_STRUCT) snprintf(val,sizeof val,"zeroinitializer");
        else if(cd->ty.kind==T_BOOL) snprintf(val,sizeof val,"%s",cd->ival?"true":"false");
        else if(cd->ty.kind==T_FLOAT||cd->ty.kind==T_HALF){
            float fv=(float)(cd->is_int?(double)cd->ival:cd->fval); unsigned bits; memcpy(&bits,&fv,4);
            if(cd->ty.kind==T_HALF) snprintf(val,sizeof val,"fptrunc (float bitcast (i32 %u to float) to half)",bits);
            else snprintf(val,sizeof val,"bitcast (i32 %u to float)",bits);
        }
        else snprintf(val,sizeof val,"%ld",cd->ival);
        fprintf(out,"@_binc_%s_%s = internal unnamed_addr addrspace(%d) global %s %s, align %d\n",
                cd->mut?"mut":"const",cd->name,cd->mut?1:2,ll,val,type_align(cd->ty.kind,0));
    }
    if(prog->nconsts) fprintf(out,"\n");
    /* struct usage: emit only structs reachable from the emitted functions +
     * const globals (dead structs may reference types from missing generated
     * includes, e.g. FNanite* headers). Closure over field types. */
    {
        int *sused=calloc(prog->nstructs?prog->nstructs:1,sizeof(int));
        /* mark from consts */
        for(size_t i=0;i<prog->nconsts;i++) if(prog->consts[i].ty.kind==T_STRUCT)
            for(size_t j=0;j<prog->nstructs;j++)
                if(!strcmp(prog->structs[j].tag,prog->consts[i].ty.struct_name)){ sused[j]=1; break; }
        /* mark from function signatures + locals */
        for(size_t fi=0;fi<prog->nfuncs;fi++){
            Function *fn=&prog->funcs[fi];
            Type *tys[8]; size_t nty=0;
            Type rts[1]; rts[0]=fn->ret;
            if(fn->ret.kind==T_STRUCT&&fn->ret.struct_name&&nty<8) tys[nty++]=&rts[0];
            for(size_t p=0;p<fn->nparams&&nty<8;p++)
                if(fn->params[p].ty.kind==T_STRUCT&&fn->params[p].ty.struct_name) tys[nty++]=&fn->params[p].ty;
            for(size_t t=0;t<nty;t++)
                for(size_t j=0;j<prog->nstructs;j++)
                    if(!strcmp(prog->structs[j].tag,tys[t]->struct_name)){ sused[j]=1; break; }
            for(size_t s=0;s<fn->body.n;s++){
                Stmt *st=&fn->body.stmts[s];
                if(st->kind==S_DECL&&st->ty.kind==T_STRUCT&&st->ty.struct_name)
                    for(size_t j=0;j<prog->nstructs;j++)
                        if(!strcmp(prog->structs[j].tag,st->ty.struct_name)){ sused[j]=1; break; }
            }
        }
        /* closure: a used struct's struct-typed fields pull their structs in */
        for(int pass=0;pass<16;pass++){
            int changed=0;
            for(size_t i=0;i<prog->nstructs;i++) if(sused[i])
                for(size_t f=0;f<prog->structs[i].nfields;f++)
                    if(prog->structs[i].fields[f].ty.kind==T_STRUCT&&prog->structs[i].fields[f].ty.struct_name)
                        for(size_t j=0;j<prog->nstructs;j++)
                            if(!strcmp(prog->structs[j].tag,prog->structs[i].fields[f].ty.struct_name)&&!sused[j]){ sused[j]=1; changed=1; }
            if(!changed) break;
        }
        /* structs in dependency order (a struct may reference another struct's
         * type; LLVM needs definitions before use — DFS post-order) */
    {
        int *seen=calloc(prog->nstructs?prog->nstructs:1,sizeof(int));
        int *done=calloc(prog->nstructs?prog->nstructs:1,sizeof(int));
        int *order=calloc(prog->nstructs?prog->nstructs:1,sizeof(int));
        size_t norder=0;
        /* find_struct index by tag */
        for(size_t i=0;i<prog->nstructs;i++){
            if(seen[i]||!sused[i]) continue;
            /* iterative DFS */
            size_t *stk=calloc(prog->nstructs?prog->nstructs:1,sizeof(size_t));
            size_t sp=0; stk[sp++]=i; seen[i]=1;
            while(sp){
                size_t cur=stk[sp-1];
                StructDef *s=&prog->structs[cur];
                int pushed=0;
                for(size_t f=0;f<s->nfields&&!pushed;f++){
                    if(s->fields[f].ty.kind!=T_STRUCT) continue;
                    for(size_t j=0;j<prog->nstructs;j++)
                        if(!strcmp(prog->structs[j].tag,s->fields[f].ty.struct_name)&&!seen[j]){
                            seen[j]=1; stk[sp++]=j; pushed=1; break;
                        }
                }
                if(!pushed){ order[norder++]=cur; done[cur]=1; sp--; }
            }
            free(stk);
        }
        for(size_t k=0;k<norder;k++){
            StructDef *s=&prog->structs[order[k]];
            if(s->is_template||!sused[order[k]]) continue;
            fprintf(out,"%%struct.%s = type { ",s->tag);
            for(size_t j=0;j<s->nfields;j++){ if(j)fprintf(out,", "); char fl[192];
                char elt[128];
                if(s->fields[j].ty.kind==T_STRUCT&&s->fields[j].ty.struct_name) snprintf(elt,sizeof elt,"%%struct.%s",s->fields[j].ty.struct_name);
                else if(s->fields[j].ty.matn) mll_of(elt,sizeof elt,s->fields[j].ty.matn,s->fields[j].ty.matm);
                else ll_of(elt,sizeof elt,s->fields[j].ty.kind,s->fields[j].ty.vecn);
                if(s->fields[j].ty.array_n){
                    if(s->fields[j].ty.array_m) snprintf(fl,sizeof fl,"[%d x [%d x %s]]",s->fields[j].ty.array_n,s->fields[j].ty.array_m,elt);
                    else snprintf(fl,sizeof fl,"[%d x %s]",s->fields[j].ty.array_n,elt);
                } else snprintf(fl,sizeof fl,"%s",elt);
                fprintf(out,"%s",fl); }
            fprintf(out," }\n");
        }
        free(seen); free(done); free(order); free(sused);
    }
    }
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

    typedef struct { int *read,*written; int np, nmeta; } KF;
    size_t kf_cap=prog->nfuncs?prog->nfuncs:1;
    KF *kf=calloc(kf_cap,sizeof(KF));
    for(volatile size_t fi=0;fi<prog->nfuncs;fi++){
        if(fi>=kf_cap){ kf_cap*=2; kf=realloc(kf,kf_cap*sizeof(KF)); memset(kf+(kf_cap/2),0,(kf_cap/2)*sizeof(KF)); }
        Function *fn=&prog->funcs[fi]; CG c={0};
        if(fn->is_template) continue; /* templates emit nothing; call sites instantiate */
        SB pr={0},bd={0}; c.pre=&pr; c.body=&bd; c.prog=prog; c.fn=fn; c.fidx=fi; c.tmp=0;
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
        char sig[65536]; size_t so=0;
        if(fn->is_kernel) so+=snprintf(sig+so,sizeof sig-so,"define void @%s(",fn->link_name?fn->link_name:fn->name);
        else { char rl[256];
            if(fn->ret.kind==T_VOID) snprintf(rl,sizeof rl,"void");
            else if(fn->ret.matn) mll_of(rl,sizeof rl,fn->ret.matn,fn->ret.matm);
            else if(fn->ret.kind==T_STRUCT){ if(fn->stage==ST_NONE) snprintf(rl,sizeof rl,"%%struct.%s",fn->ret.struct_name);
                else { StructDef *sd=find_struct(prog,fn->ret.struct_name); if(!sd){ die(fn->line,"internal: struct return %s not found",fn->ret.struct_name); } stage_lit_type(rl,sizeof rl,sd); } }
            else ll_of(rl,sizeof rl,fn->ret.kind,fn->ret.vecn);
            so+=snprintf(sig+so,sizeof sig-so,"define %s%s @%s(",fn->stage==ST_NONE?"internal ":"",rl,fn->link_name?fn->link_name:fn->name); }
        int emitted=0;
        for(size_t i=0;i<fn->nparams;i++){ Param *p=&fn->params[i];
            if(p->ty.array_n) continue; /* shared arrays are module globals, not ABI args */
            if(fn->stage!=ST_NONE && p->ty.kind==T_STRUCT && !p->ty.is_ptr){
                /* stage-in struct (vertex attributes or fragment interpolants):
                 * unpacked as separate arguments */
                StructDef *sd=find_struct(prog,p->ty.struct_name);
                for(size_t f=0;f<sd->nfields;f++){ if(struct_empty(prog,sd->fields[f].ty.struct_name)) continue;
                    char fl[64]; type_ll(fl,sizeof fl,sd->fields[f].ty.kind,sd->fields[f].ty.struct_name,sd->fields[f].ty.vecn);
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
            else if(fn->is_kernel){ char ll[32]; pll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn,p->ty.matn,p->ty.matm);
                so+=snprintf(sig+so,sizeof sig-so,"%s addrspace(2)* nocapture noundef readonly align %d dereferenceable(%d) %%_%s",ll,tal(p->ty.kind,p->ty.vecn,p->ty.matn),tsz(p->ty.kind,p->ty.vecn,p->ty.matn),p->name); }
            else { char ll[96];
                if(p->ty.kind==T_STRUCT) snprintf(ll,sizeof ll,"%%struct.%s",p->ty.struct_name);
                else pll_of(ll,sizeof ll,p->ty.kind,p->ty.vecn,p->ty.matn,p->ty.matm);
                so+=snprintf(sig+so,sizeof sig-so,"%s noundef %%_%s",ll,p->name); } }
        if(fn->is_kernel && c.explicit_domain && c.coord_param>=0){ Param *cp=&fn->params[c.coord_param]; char cl[32]; coord_ll(&cp->ty,cl,sizeof cl);
            so+=snprintf(sig+so,sizeof sig-so,", %s noundef %%_%s_local, %s noundef %%_%s_group",cl,cp->name,cl,cp->name); }
        if(fn->is_kernel && !c.explicit_domain){ if(emitted)so+=snprintf(sig+so,sizeof sig-so,", ");
            so+=snprintf(sig+so,sizeof sig-so,"i32 noundef %%_id"); }
        if(fn->is_kernel) so+=snprintf(sig+so,sizeof sig-so,") local_unnamed_addr");
        else so+=snprintf(sig+so,sizeof sig-so,")");
        /* name the entry block FIRST (before any prologue instructions), so
         * short-circuit phis can reference it without splitting the block */
        { char *entry=malloc(32); snprintf(entry,32,"bb%d:\n",c.lblc);
            sb_put(c.pre,entry); free(entry); c.curbb=c.lblc; c.lblc++; }
        if(fn->is_kernel && !c.explicit_domain){ c.idx=malloc(16); snprintf(c.idx,16,"%%t%d",c.tmp++);
            sb_printf(c.pre,"  %s = zext i32 %%_id to i64\n",c.idx); }
        else if(c.explicit_domain && c.coord_param>=0 && fn->params[c.coord_param].ty.coordn==1){
            c.idx=malloc(32); snprintf(c.idx,32,"%%_%s",fn->params[c.coord_param].name);
            const char *z=newtmp(&c); sb_printf(c.pre,"  %s = zext i32 %s to i64\n",z,c.idx); c.idx=(char*)z;
        }
        /* plain-function struct-by-value params: copy the value argument into a local */
        if(fn->stage==ST_NONE) for(size_t pi2=0;pi2<fn->nparams;pi2++){ Param *p=&fn->params[pi2];
            if(p->ty.kind!=T_STRUCT||p->ty.is_ptr) continue;
            StructDef *sd=find_struct(prog,p->ty.struct_name);
            if(!sd) die(0,"unknown struct %s",p->ty.struct_name);
            int sal; struct_layout(sd,&sal);
            char *slot=newtmp(&c); sb_printf(c.pre,"  %s = alloca %%struct.%s, align %d\n",slot,p->ty.struct_name,sal);
            char an[64]; snprintf(an,sizeof an,"%%_%s",p->name);
            char sn[64]; snprintf(sn,sizeof sn,"%%struct.%s",p->ty.struct_name);
            emit(&c,"  store %s %s, %s* %s, align %d\n",sn,an,sn,slot,sal);
            c.locs=realloc(c.locs,(c.nlocs+1)*sizeof(Loc));
            c.locs[c.nlocs++]=(Loc){p->name,slot,T_STRUCT,p->ty.struct_name,0,0,0,0,0};
        }
        /* stage-in struct (vertex or fragment): unpack the argument registers into a struct local */
        if(fn->stage!=ST_NONE) for(size_t pi2=0;pi2<fn->nparams;pi2++){ Param *p=&fn->params[pi2];
            if(p->ty.kind!=T_STRUCT||p->ty.is_ptr) continue;
            StructDef *sd=find_struct(prog,p->ty.struct_name);
            int sal; struct_layout(sd,&sal);
            char *slot=newtmp(&c); sb_printf(c.pre,"  %s = alloca %%struct.%s, align %d\n",slot,p->ty.struct_name,sal);
            for(size_t f=0;f<sd->nfields;f++){ if(struct_empty(prog,sd->fields[f].ty.struct_name)) continue;
                char pty[96]; pty_str(pty,sizeof pty,sd->fields[f].ty.kind,sd->fields[f].ty.struct_name,0,1,sd->fields[f].ty.vecn,0);
                char ll[64];
                if(sd->fields[f].ty.kind==T_STRUCT) snprintf(ll,sizeof ll,"%%struct.%s",sd->fields[f].ty.struct_name);
                else ll_of(ll,sizeof ll,sd->fields[f].ty.kind,sd->fields[f].ty.vecn);
                char sp[64]; snprintf(sp,sizeof sp,"%%struct.%s*",p->ty.struct_name);
                const char *fp=newtmp(&c);
                emit(&c,"  %s = getelementptr inbounds %%struct.%s, %s %s, i64 0, i32 %zu\n",fp,p->ty.struct_name,sp,slot,f);
                char an[64]; snprintf(an,sizeof an,"%%_%s.%zu",p->name,f);
                emit(&c,"  store %s %s, %s %s, align %d\n",ll,an,pty,fp,type_align(sd->fields[f].ty.kind,sd->fields[f].ty.vecn));
            }
            c.locs=realloc(c.locs,(c.nlocs+1)*sizeof(Loc));
            c.locs[c.nlocs++]=(Loc){p->name,slot,T_STRUCT,p->ty.struct_name,0,0,0,0,0};
        }
        /* top-level body: re-derive the Block after every statement, because a
         * template instantiation reallocs prog->funcs and moves the Function's
         * body field (nested blocks live in the heap-stable stmts array) */
        for(size_t si=0;;si++){ Block *b=&c.fn->body; if(si>=b->n) break;
            if(getenv("BINC_DEBUG_IW")&&!strcmp(c.fn->name,"GetPrimitive_PerObjectGBufferData_FromFlags")) fprintf(stderr,"DBG stmt %zu: kind=%d %s\n",si,b->stmts[si].kind,b->stmts[si].name?b->stmts[si].name:"");
            gen_stmt(&c,&b->stmts[si]); }
        if(!c.term){ if(c.fn->ret.kind==T_VOID) emit(&c,"  ret void\n");
            else if(c.fn->ret.kind==T_STRUCT){ StructDef *sd=find_struct(prog,c.fn->ret.struct_name);
                char lt[256]; stage_lit_type(lt,sizeof lt,sd); emit(&c,"  ret %s undef\n",lt); }
            else { char rl[32]; ll_of(rl,sizeof rl,c.fn->ret.kind,c.fn->ret.vecn);
                emit(&c,"  ret %s %s\n",rl,c.fn->ret.vecn>1?"zeroinitializer":
                    c.fn->ret.kind==T_FLOAT||c.fn->ret.kind==T_HALF?"0.000000e+00":c.fn->ret.kind==T_BOOL?"false":"0"); } }
        /* a body containing a barrier must be convergent and cannot be nosync (#1); else #0 */
        fprintf(out,"%s #%d {\n",sig,c.uses_sync?1:0);
        fwrite(pr.p,1,pr.n,out); fwrite(bd.p,1,bd.n,out);
        fprintf(out,"}\n\n");
    }
    g_recover=NULL;
    if(g_uses_discard) fprintf(out,"declare void @air.discard_fragment() local_unnamed_addr\n");
    if(atomic_add_used[0]) fprintf(out,"declare float @air.atomic.global.add.f32(float addrspace(1)*, float, i32, i32, i32, i1) local_unnamed_addr\n");
    if(atomic_add_used[1]) fprintf(out,"declare i32 @air.atomic.global.add.i32(i32 addrspace(1)*, i32, i32, i32, i32, i1) local_unnamed_addr\n");
    /* texture intrinsics + the opaque texture/sampler types */
    {
        int any_tex_param=0;
        for(size_t fi=0;fi<prog->nfuncs&&!any_tex_param;fi++)
            for(size_t pi=0;pi<prog->funcs[fi].nparams;pi++)
                if(prog->funcs[fi].params[pi].ty.kind==T_TEXTURE||prog->funcs[fi].params[pi].ty.kind==T_SAMPLER){ any_tex_param=1; break; }
        if(get_samp_used||tex_read_used[0]||tex_read_used[1]||tex_read_used[2]||tex_read_used[3]||
           tex_write_used[0]||tex_write_used[1]||tex_write_used[2]||tex_write_used[3]||
           tex_sample_used[0]||tex_sample_used[1]||tex_sample_used[2]||tex_sample_used[3]||any_tex_param){
        fprintf(out,"%%struct._texture_2d_t = type opaque\n%%struct._sampler_t = type opaque\n");
        if(get_samp_used) fprintf(out,"declare %%struct._sampler_t addrspace(2)* @air.get_read_sampler() local_unnamed_addr\n");
        static const char *sufs[4]={"v4f32","v4f16","v4i32","v4u32"};
        static const char *vecs[4]={"<4 x float>","<4 x half>","<4 x i32>","<4 x i32>"};
        for(int i=0;i<4;i++){
            if(tex_read_used[i]) fprintf(out,"declare { %s, i8 } @air.read_texture_2d.%s(%%struct._texture_2d_t addrspace(1)* nocapture readonly, %%struct._sampler_t addrspace(2)*, <2 x i32>, <2 x i32>, i32, i32) local_unnamed_addr\n",vecs[i],sufs[i]);
            if(tex_write_used[i]) fprintf(out,"declare void @air.write_texture_2d.%s(%%struct._texture_2d_t addrspace(1)* nocapture, <2 x i32>, %s, i32, i32) local_unnamed_addr\n",sufs[i],vecs[i]);
            if(tex_sample_used[i]) fprintf(out,"declare { %s, i8 } @air.sample_texture_2d.%s(%%struct._texture_2d_t addrspace(1)* nocapture readonly, %%struct._sampler_t addrspace(2)* nocapture readonly, <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr\n",vecs[i],sufs[i]);
            if(tex_sample_cube_used[i]) fprintf(out,"declare { %s, i8 } @air.sample_texture_cube.%s(%%struct._texture_2d_t addrspace(1)* nocapture readonly, %%struct._sampler_t addrspace(2)* nocapture readonly, <3 x float>, i1, float, float, i32) local_unnamed_addr\n",vecs[i],sufs[i]);
        }
        }
    }
    /* declares for the builtins that were actually used (deduped by AIR name:
     * aliases like min/fmin share air.fast_fmin.f32) */
    for(size_t b=0;b<sizeof builtins/sizeof *builtins;b++){ if(!builtin_used[b]) continue; Builtin *bi=&builtins[b];
        int dup=0; for(size_t q=0;q<b;q++) if(builtin_used[q]&&!strcmp(builtins[q].ll,bi->ll)){ dup=1; break; }
        if(dup) continue;
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
            if(sf->stage!=ST_NONE && sf->params[a].ty.kind==T_STRUCT && !sf->params[a].ty.is_ptr){
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
    meta_emit(&meta,"!0 = !{i32 2, !\"SDK Version\", [2 x i32] [i32 %d, i32 0]}\n!1 = !{i32 1, !\"wchar_size\", i32 4}\n!2 = !{i32 7, !\"air.max_device_buffers\", i32 31}\n!3 = !{i32 7, !\"air.max_constant_buffers\", i32 31}\n!4 = !{i32 7, !\"air.max_threadgroup_buffers\", i32 31}\n!5 = !{i32 7, !\"air.max_textures\", i32 128}\n!6 = !{i32 7, !\"air.max_samplers\", i32 16}\n!7 = !{!\"air.compile.denorms_disable\"}\n!8 = !{!\"air.compile.fast_math_enable\"}\n!9 = !{!\"BinC compiler v0.0.1 (bootstrap, in C)\"}\n!10 = !{i32 2, i32 %d, i32 0}\n!11 = !{!\"Metal\", i32 4, i32 1, i32 0}\n!12 = !{!\"binc\"}\n",g_air_sdk,g_air_minor);
    ki=0;
    for(size_t fi=0;fi<prog->nfuncs;fi++){ if(!prog->funcs[fi].is_kernel) continue;
        emit_kernel_meta(&meta,prog,&prog->funcs[fi],&km[ki],kf[fi].read,kf[fi].written,kf[fi].nmeta,kf[fi].np);
        ki++; }
    /* stage emission preserves the previous grouping: vertices first, then fragments */
    for(size_t s=0;s<ns;s++) if(sm[s].fn->stage==ST_VERTEX) emit_stage_meta(&meta,prog,sm[s].fn,&sm[s]);
    for(size_t s=0;s<ns;s++) if(sm[s].fn->stage==ST_FRAGMENT) emit_stage_meta(&meta,prog,sm[s].fn,&sm[s]);
    fwrite(meta.sb.p,1,meta.sb.n,out);
}
