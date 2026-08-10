/* hlsl_lower.c — lowering bridge: HLSLProg -> shared BinC Program.
 *
 * The whole point of the bridge: reuse the existing AIR backend untouched.
 * Entry selection (-E), stage selection (-T), semantic mapping, compute
 * thread-id rewriting, and render-stage synthesis happen here. */
#include "binc.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static Program g_prog; /* survives die()'s longjmp */

static int sem_eq(const char *a, const char *b){
    if(!a||!b) return 0;
    while(*a&&*b){ if(tolower((unsigned char)*a)!=tolower((unsigned char)*b)) return 0; a++; b++; }
    return *a==*b;
}
static int sem_pref(const char *sem, const char *pre){
    if(!sem) return 0;
    return strncasecmp(sem,pre,strlen(pre))==0;
}

/* map an HLSL semantic to BinC field attributes.
 * vertex_input: POSITION inputs are plain attributes (locn0); outputs and
 * fragment inputs keep SV_Position as the position builtin. */
static void sem_to_attr(const char *sem, int *attr, int *idx, int vertex_input){
    *attr=0; *idx=0;
    if(!sem) return;
    if(vertex_input){
        if(sem_pref(sem,"POSITION")){ *attr=5; *idx=0; return; }
        if(sem_pref(sem,"TEXCOORD")){ *attr=5; *idx=atoi(sem+8)+1; return; } /* leave locn0 for POSITION */
        if(sem_pref(sem,"COLOR")){ *attr=5; *idx=8+atoi(sem+5); return; }
        if(sem_eq(sem,"NORMAL")){ *attr=5; *idx=4; return; }
        if(sem_eq(sem,"TANGENT")){ *attr=5; *idx=5; return; }
        if(sem_eq(sem,"BINORMAL")){ *attr=5; *idx=6; return; }
        if(sem_pref(sem,"SV_")){ *attr=5; *idx=0; return; }
        *attr=5; *idx=0; return; /* unknown input semantic: attribute 0 */
    }
    if(sem_eq(sem,"SV_Position")){ *attr=1; return; }
    if(sem_pref(sem,"POSITION")){ *attr=1; return; } /* D3D9 VS out: POSITION == air.position */
    if(sem_pref(sem,"SV_Target")){ *attr=3; *idx=atoi(sem+9); return; }
    if(sem_eq(sem,"SV_Depth")){ *attr=4; return; }
    if(sem_pref(sem,"TEXCOORD")){ *attr=5; *idx=atoi(sem+8)+1; return; } /* +1: keep locn0 for POSITION (VS outputs / PS inputs) */
    if(sem_pref(sem,"COLOR")){ *attr=5; *idx=8+atoi(sem+5); return; } /* +8: keep locn0-7 for POSITION/TEXCOORD/NORMAL/TANGENT */
    if(sem_eq(sem,"NORMAL")){ *attr=5; *idx=4; return; }
    if(sem_eq(sem,"TANGENT")){ *attr=5; *idx=5; return; }
    if(sem_eq(sem,"BINORMAL")){ *attr=5; *idx=6; return; }
    if(sem_pref(sem,"SV_")){ *attr=5; *idx=0; return; }
    *attr=5; *idx=0;
}

/* ---- expression/statement rewriting ---- */
typedef struct { const char **names; size_t n; const char *field; const char *base; Expr *expr; } Rewrite;

static Expr *rw_copy(Expr *e){
    if(!e) return NULL;
    Expr *n=calloc(1,sizeof(Expr)); *n=*e;
    n->name=e->name?strdup(e->name):NULL;
    n->field=e->field?strdup(e->field):NULL;
    n->operand=rw_copy(e->operand); n->lhs=rw_copy(e->lhs); n->rhs=rw_copy(e->rhs);
    n->callee=rw_copy(e->callee);
    if(e->nargs){ n->args=calloc(e->nargs,sizeof(Expr*)); for(size_t i=0;i<e->nargs;i++) n->args[i]=rw_copy(e->args[i]); }
    return n;
}

static int rw_matches(const Rewrite *rw, const char *name){
    if(!name) return 0;
    for(size_t i=0;i<rw->n;i++) if(rw->names[i]&&!strcmp(rw->names[i],name)) return 1;
    return 0;
}
static void rw_expr(Expr *e, const Rewrite *rw){
    if(!e) return;
    if(e->kind==E_IDENT&&rw_matches(rw,e->name)){
        if(rw->expr){ /* E_IDENT(name) -> a computed expression (derived thread ids) */
            Expr *n=rw_copy(rw->expr);
            *e=*n;
            return;
        }
        /* E_IDENT(name) -> E_FIELD(E_IDENT(base), field); do not recurse */
        Expr *base=E(E_IDENT,e->line,e->col); base->name=strdup(rw->base?rw->base:e->name);
        Expr *n=E(E_FIELD,e->line,e->col); n->operand=base; n->field=strdup(rw->field);
        *e=*n;
        return;
    }
    rw_expr(e->operand,rw); rw_expr(e->lhs,rw); rw_expr(e->rhs,rw);
    if(e->callee) rw_expr(e->callee,rw);
    for(size_t i=0;i<e->nargs;i++) rw_expr(e->args[i],rw);
}
static void rw_block(Block *b, const Rewrite *rw);
static void rw_stmt(Stmt *st, const Rewrite *rw){
    switch(st->kind){
    case S_EXPR: rw_expr(st->expr,rw); break;
    case S_DECL: rw_expr(st->init,rw); break;
    case S_RETURN: rw_expr(st->expr,rw); break;
    case S_IF: rw_expr(st->cond,rw); rw_block(&st->then_b,rw); rw_block(&st->else_b,rw); break;
    case S_WHILE: case S_DOWHILE: rw_expr(st->cond,rw); rw_block(&st->then_b,rw); break;
    case S_FOR:
        if(st->for_init) rw_stmt(st->for_init,rw);
        rw_expr(st->for_cond,rw); rw_expr(st->for_incr,rw); rw_block(&st->then_b,rw); break;
    case S_SWITCH:
        rw_expr(st->sw_cond,rw);
        for(size_t i=0;i<st->ncases;i++){ rw_expr(st->cases[i].val,rw); rw_block(&st->cases[i].body,rw); }
        rw_block(&st->def_body,rw); break;
    case S_BLOCK: rw_block(&st->then_b,rw); break;
    default: break;
    }
}
static void rw_block(Block *b, const Rewrite *rw){
    for(size_t i=0;i<b->n;i++) rw_stmt(&b->stmts[i],rw);
}

/* HLSL uniform-buffer structs may expose resource members (for example
 * View.PerlinNoiseGradientTexture) even though Metal requires those opaque
 * resources to remain separate function parameters. Infer the resource kind
 * from the formal parameter of the call receiving the field, flatten the
 * path, and keep the value-struct ABI free of opaque members. */
typedef struct { char *path; char *flat; Type ty; } HLSLResourceField;

static int resource_field_path(Expr *e, char *out, size_t cap){
    if(!e) return 0;
    if(e->kind==E_IDENT&&e->name){ snprintf(out,cap,"%s",e->name); return 1; }
    if(e->kind==E_FIELD&&e->field){
        char base[256];
        if(!resource_field_path(e->operand,base,sizeof base)) return 0;
        snprintf(out,cap,"%s.%s",base,e->field);
        return 1;
    }
    return 0;
}

static HLSLFunc *hlsl_find_func(HLSLProg *hp, const char *name){
    if(!name) return NULL;
    for(size_t i=0;i<hp->nfuncs;i++) if(!strcmp(hp->funcs[i].name,name)) return &hp->funcs[i];
    return NULL;
}

static void resource_field_add(HLSLResourceField **out, size_t *n, Expr *actual, Type ty){
    char path[256]; if(!resource_field_path(actual,path,sizeof path)||!strchr(path,'.')) return;
    for(size_t i=0;i<*n;i++) if(!strcmp((*out)[i].path,path)) return;
    HLSLResourceField *v=realloc(*out,(*n+1)*sizeof **out); if(!v) return;
    *out=v;
    HLSLResourceField *r=&v[(*n)++]; r->path=strdup(path); r->ty=ty;
    char flat[256]; size_t j=0;
    for(size_t k=0;path[k]&&j+1<sizeof flat;k++) flat[j++]=path[k]=='.'?'_':path[k];
    flat[j]=0; r->flat=strdup(flat);
}

static void resource_field_collect_expr(Expr *e, HLSLProg *hp, HLSLResourceField **out, size_t *n){
    if(!e) return;
    if(e->kind==E_CALL&&e->name&&!e->callee){
        HLSLFunc *hf=hlsl_find_func(hp,e->name);
        if(hf) for(size_t i=0;i<e->nargs&&i<hf->np;i++){
            TypeKind k=hf->params[i].ty.kind;
            if(k==T_TEXTURE||k==T_SAMPLER) resource_field_add(out,n,e->args[i],hf->params[i].ty);
        }
    }
    resource_field_collect_expr(e->operand,hp,out,n);
    resource_field_collect_expr(e->lhs,hp,out,n);
    resource_field_collect_expr(e->rhs,hp,out,n);
    resource_field_collect_expr(e->callee,hp,out,n);
    for(size_t i=0;i<e->nargs;i++) resource_field_collect_expr(e->args[i],hp,out,n);
}

static void resource_field_collect_block(Block *b, HLSLProg *hp, HLSLResourceField **out, size_t *n){
    for(size_t i=0;i<b->n;i++){
        Stmt *st=&b->stmts[i];
        resource_field_collect_expr(st->expr,hp,out,n);
        resource_field_collect_expr(st->init,hp,out,n);
        resource_field_collect_expr(st->cond,hp,out,n);
        resource_field_collect_expr(st->for_incr,hp,out,n);
        if(st->for_init) resource_field_collect_expr(st->for_init->expr,hp,out,n);
        resource_field_collect_block(&st->then_b,hp,out,n);
        resource_field_collect_block(&st->else_b,hp,out,n);
        for(size_t c=0;c<st->ncases;c++){
            resource_field_collect_expr(st->cases[c].val,hp,out,n);
            resource_field_collect_block(&st->cases[c].body,hp,out,n);
        }
        resource_field_collect_block(&st->def_body,hp,out,n);
    }
}

static Expr *resource_field_rewrite_expr(Expr *e, HLSLResourceField *rf, size_t nrf){
    if(!e) return NULL;
    e->operand=resource_field_rewrite_expr(e->operand,rf,nrf);
    e->lhs=resource_field_rewrite_expr(e->lhs,rf,nrf);
    e->rhs=resource_field_rewrite_expr(e->rhs,rf,nrf);
    e->callee=resource_field_rewrite_expr(e->callee,rf,nrf);
    for(size_t i=0;i<e->nargs;i++) e->args[i]=resource_field_rewrite_expr(e->args[i],rf,nrf);
    if(e->kind==E_FIELD){
        char path[256];
        if(resource_field_path(e,path,sizeof path)) for(size_t i=0;i<nrf;i++) if(!strcmp(path,rf[i].path)){
            Expr *n=E(E_IDENT,e->line,e->col); n->name=strdup(rf[i].flat); return n;
        }
    }
    return e;
}

static void resource_field_rewrite_block(Block *b, HLSLResourceField *rf, size_t nrf){
    for(size_t i=0;i<b->n;i++){
        Stmt *st=&b->stmts[i];
        st->expr=resource_field_rewrite_expr(st->expr,rf,nrf);
        st->init=resource_field_rewrite_expr(st->init,rf,nrf);
        st->cond=resource_field_rewrite_expr(st->cond,rf,nrf);
        st->for_incr=resource_field_rewrite_expr(st->for_incr,rf,nrf);
        if(st->for_init) st->for_init->expr=resource_field_rewrite_expr(st->for_init->expr,rf,nrf);
        resource_field_rewrite_block(&st->then_b,rf,nrf);
        resource_field_rewrite_block(&st->else_b,rf,nrf);
        for(size_t c=0;c<st->ncases;c++){
            st->cases[c].val=resource_field_rewrite_expr(st->cases[c].val,rf,nrf);
            resource_field_rewrite_block(&st->cases[c].body,rf,nrf);
        }
        resource_field_rewrite_block(&st->def_body,rf,nrf);
    }
}

/* ---- D3D9 sm3 tex2D family: `tex2D(s, uv)` -> `s.tex.Sample(s, uv)` ----
 * The sampler global records the texture it was bound to in sampler_state
 * (Texture = <name>) or by register convention; rewrite the intrinsic call
 * into the texture-method form the codegen already lowers. */
static void tex2d_expr(Expr *e, HLSLProg *hp);
static void tex2d_block(Block *b, HLSLProg *hp);
static void tex2d_stmt(Stmt *st, HLSLProg *hp){
    switch(st->kind){
    case S_EXPR: tex2d_expr(st->expr,hp); break;
    case S_DECL: tex2d_expr(st->init,hp); break;
    case S_RETURN: tex2d_expr(st->expr,hp); break;
    case S_IF: tex2d_expr(st->cond,hp); tex2d_block(&st->then_b,hp); tex2d_block(&st->else_b,hp); break;
    case S_WHILE: case S_DOWHILE: tex2d_expr(st->cond,hp); tex2d_block(&st->then_b,hp); break;
    case S_FOR:
        if(st->for_init) tex2d_stmt(st->for_init,hp);
        tex2d_expr(st->for_cond,hp); tex2d_expr(st->for_incr,hp); tex2d_block(&st->then_b,hp); break;
    case S_SWITCH:
        tex2d_expr(st->sw_cond,hp);
        for(size_t i=0;i<st->ncases;i++){ tex2d_expr(st->cases[i].val,hp); tex2d_block(&st->cases[i].body,hp); }
        tex2d_block(&st->def_body,hp); break;
    case S_BLOCK: tex2d_block(&st->then_b,hp); break;
    default: break;
    }
}
static void tex2d_block(Block *b, HLSLProg *hp){
    for(size_t i=0;i<b->n;i++) tex2d_stmt(&b->stmts[i],hp);
}
static const char *sampler_tex_name(HLSLProg *hp, const char *sampler, int *reg_out){
    for(size_t i=0;i<hp->nglobals;i++){
        HLSLGlobal *g=&hp->globals[i];
        if(!strcmp(g->name,sampler)&&g->ty.kind==T_SAMPLER){
            if(reg_out) *reg_out=g->reg;
            return g->tex_name;
        }
    }
    return NULL;
}
static void tex2d_expr(Expr *e, HLSLProg *hp){
    if(!e) return;
    tex2d_expr(e->operand,hp); tex2d_expr(e->lhs,hp); tex2d_expr(e->rhs,hp);
    if(e->callee) tex2d_expr(e->callee,hp);
    for(size_t i=0;i<e->nargs;i++) tex2d_expr(e->args[i],hp);
    if(e->kind!=E_CALL||e->callee||e->nargs<1) return;
    const char *nm=e->name;
    if(!nm) return;
    int is_tex2d = !strcmp(nm,"tex2D")||!strcmp(nm,"texCUBE")||!strcmp(nm,"tex2Dproj");
    int is_lod   = !strcmp(nm,"tex2Dlod");
    if(!(is_tex2d||is_lod||!strcmp(nm,"tex2Dgrad"))) return;
    Expr *s0=e->args[0];
    if(s0->kind!=E_IDENT) return;
    int sreg=-1; const char *tn=sampler_tex_name(hp,s0->name,&sreg);
    if(!tn){ /* register convention: sampler sN binds texture tN */
        if(sreg>=0){
            for(size_t i=0;i<hp->nglobals;i++){
                HLSLGlobal *g=&hp->globals[i];
                if(g->ty.kind==T_TEXTURE&&g->reg==sreg){ tn=g->name; break; }
            }
        }
    }
    if(!tn) return; /* unresolved: leave the call as-is (codegen will die) */
    Expr *op=E(E_IDENT,s0->line,s0->col); op->name=strdup(tn);
    Expr *f=E(E_FIELD,s0->line,s0->col); f->operand=op;
    if(is_tex2d){ f->field=strdup("Sample"); e->name=strdup("Sample"); }
    else { f->field=strdup("SampleLevel"); e->name=strdup("SampleLevel"); }
    e->callee=f;
    /* tex2Dproj: uv is float3/float4 with the projection in the last lane:
     * tex2Dproj(s, uv) == Sample(s, uv.xy / uv.z) (or .xy / .w) */
    if(!strcmp(nm,"tex2Dproj")&&e->nargs>=2&&e->args[1]){
        Expr *uv=e->args[1];
        Expr *xy=E(E_FIELD,uv->line,uv->col); xy->operand=uv; xy->field=strdup("xy");
        Expr *w=E(E_FIELD,uv->line,uv->col); w->operand=uv; w->field=strdup("w");
        Expr *d=E(E_BIN,uv->line,uv->col); d->bop=B_DIV; d->lhs=xy; d->rhs=w;
        e->args[1]=d;
    }
    /* tex2Dlod(s, float4(uv, 0, lod)) == SampleLevel(s, uv.xy, lod) */
    if(!strcmp(nm,"tex2Dlod")&&e->nargs>=2&&e->args[1]){
        Expr *v=e->args[1];
        Expr *xy=E(E_FIELD,v->line,v->col); xy->operand=v; xy->field=strdup("xy");
        Expr *w=E(E_FIELD,v->line,v->col); w->operand=v; w->field=strdup("w");
        Expr *args2[3]; args2[0]=s0; args2[1]=xy; args2[2]=w;
        e->nargs=3; memcpy(e->args,args2,3*sizeof(Expr*));
        e->name=strdup("SampleLevel"); f->field=strdup("SampleLevel");
    }
    /* tex2Dgrad(s, uv, dx, dy) — SampleLevel with lod 0 (gradient path: see codegen) */
    if(!strcmp(nm,"tex2Dgrad")&&e->nargs>=4){
        Expr *z=E(E_FCONST,e->args[1]->line,e->args[1]->col); z->fval=0.0;
        Expr *args2[3]; args2[0]=s0; args2[1]=e->args[1]; args2[2]=z;
        e->nargs=3; memcpy(e->args,args2,3*sizeof(Expr*));
        e->name=strdup("SampleLevel"); f->field=strdup("SampleLevel");
    }
}

/* rewrite `return e;` into `__out.field = e; return __out;` (fragment stage-out) */
static void rr_block(Block *b, const char *out, const char *field);
static void rr_stmt(Stmt *st, const char *out, const char *field){
    switch(st->kind){
    case S_IF: rr_block(&st->then_b,out,field); rr_block(&st->else_b,out,field); break;
    case S_WHILE: case S_DOWHILE: rr_block(&st->then_b,out,field); break;
    case S_FOR:
        if(st->for_init) rr_stmt(st->for_init,out,field);
        rr_block(&st->then_b,out,field); break;
    case S_SWITCH:
        for(size_t i=0;i<st->ncases;i++) rr_block(&st->cases[i].body,out,field);
        rr_block(&st->def_body,out,field); break;
    case S_BLOCK: rr_block(&st->then_b,out,field); break;
    default: break;
    }
}
static void rr_block(Block *b, const char *out, const char *field){
    for(size_t i=0;i<b->n;i++){
        Stmt *st=&b->stmts[i];
        if(st->kind==S_RETURN&&st->expr){
            Expr *lhs=E(E_FIELD,st->line,st->col);
            Expr *base=E(E_IDENT,st->line,st->col); base->name=strdup(out);
            lhs->operand=base; lhs->field=strdup(field);
            Expr *as=E(E_ASSIGN,st->line,st->col); as->aop=A_ASSIGN; as->operand=lhs; as->rhs=st->expr;
            Stmt sx={0}; sx.kind=S_EXPR; sx.line=st->line; sx.col=st->col; sx.expr=as;
            Stmt sr={0}; sr.kind=S_RETURN; sr.line=st->line; sr.col=st->col;
            sr.expr=E(E_IDENT,st->line,st->col); sr.expr->name=strdup(out);
            *st=sx;
            b->stmts=realloc(b->stmts,(b->n+1)*sizeof(Stmt));
            memmove(&b->stmts[i+2],&b->stmts[i+1],(b->n-i-1)*sizeof(Stmt));
            b->stmts[i+1]=sr; b->n++;
            i++;
        } else rr_stmt(st,out,field);
    }
}

/* ---- inout helpers (Phase 5): void f(inout T a, ...) -> T f(T a, ...) ----
 * The call sites `f(a, b, ...)` become `a = f(b, ...)` (the inout arg is the
 * receiver, removed from the call). The function body gets `return a;`
 * appended so the modified value flows out. */
static void iw_expr(Expr *e, const char *fnname, int io);
static void iw_block(Block *b, const char *fnname, int io);
static void sr_expr(Expr *e, const char *spname, const char *sfield, const char *coord, const char *fld);
static void sr_block(Block *b, const char *spname, const char *sfield, const char *coord, const char *fld){
    for(size_t i=0;i<b->n;i++){
        Stmt *st=&b->stmts[i];
        if(st->expr) sr_expr(st->expr,spname,sfield,coord,fld);
        if(st->init) sr_expr(st->init,spname,sfield,coord,fld);
        if(st->cond) sr_expr(st->cond,spname,sfield,coord,fld);
        if(st->for_incr) sr_expr(st->for_incr,spname,sfield,coord,fld);
        if(st->for_init&&st->for_init->expr) sr_expr(st->for_init->expr,spname,sfield,coord,fld);
        sr_block(&st->then_b,spname,sfield,coord,fld); sr_block(&st->else_b,spname,sfield,coord,fld);
        for(size_t c=0;c<st->ncases;c++){ sr_expr(st->cases[c].val,spname,sfield,coord,fld); sr_block(&st->cases[c].body,spname,sfield,coord,fld); }
        sr_block(&st->def_body,spname,sfield,coord,fld);
    }
}
/* stage_input.<svfield> -> <coord>.<global|local|group> (SPIRV-Cross compute inputs) */
static void sr_expr(Expr *e, const char *spname, const char *sfield, const char *coord, const char *fld){
    if(!e) return;
    if(e->kind==E_FIELD&&e->operand&&e->operand->kind==E_IDENT&&
       !strcmp(e->operand->name,spname)&&!strcmp(e->field,sfield)){
        if(getenv("BINC_DEBUG_D3D9")) fprintf(stderr,"DBG sr-repl: %s.%s -> %s.%s (line %d)\n",spname,sfield,coord,fld,e->line);
        Expr *base=E(E_IDENT,e->line,e->col); base->name=strdup(coord);
        Expr *n=E(E_FIELD,e->line,e->col); n->operand=base; n->field=strdup(fld);
        *e=*n;
        return;
    }
    sr_expr(e->operand,spname,sfield,coord,fld); sr_expr(e->lhs,spname,sfield,coord,fld); sr_expr(e->rhs,spname,sfield,coord,fld);
    if(e->callee) sr_expr(e->callee,spname,sfield,coord,fld);
    for(size_t i=0;i<e->nargs;i++) sr_expr(e->args[i],spname,sfield,coord,fld);
}
static void rn_expr(Expr *e){ /* HLSL barriers -> the sync builtin */
    if(!e) return;
    if(e->kind==E_CALL&&e->name&&(!strcmp(e->name,"GroupMemoryBarrierWithGroupSync")||!strcmp(e->name,"GroupMemoryBarrier")||!strcmp(e->name,"DeviceMemoryBarrier")||!strcmp(e->name,"AllMemoryBarrierWithGroupSync")))
        e->name=strdup("sync");
    rn_expr(e->operand); rn_expr(e->lhs); rn_expr(e->rhs);
    if(e->callee) rn_expr(e->callee);
    for(size_t i=0;i<e->nargs;i++) rn_expr(e->args[i]);
}
static void rn_block(Block *b){ for(size_t i=0;i<b->n;i++){
    Stmt *st=&b->stmts[i];
    if(st->expr) rn_expr(st->expr);
    if(st->init) rn_expr(st->init);
    if(st->cond) rn_expr(st->cond);
    if(st->for_incr) rn_expr(st->for_incr);
    if(st->for_init) rn_expr(st->for_init->expr);
    rn_block(&st->then_b); rn_block(&st->else_b);
    for(size_t c=0;c<st->ncases;c++){ rn_expr(st->cases[c].val); rn_block(&st->cases[c].body); }
    rn_block(&st->def_body);
} }
/* InterlockedAdd/Min/Max/And/Or/Xor/Exchange(dst[i], v) -> the atomic-method
 * form dst.add(v). The target buffer gets its element address; the codegen
 * atomic machinery handles the rest. Element indices beyond 0 are deferred
 * (the method form indexes element 0). */
static void ax_expr(Expr *e){
    if(!e) return;
    if(e->kind==E_CALL&&e->name&&!strncmp(e->name,"Interlocked",11)){
        if(e->nargs<2) die(0,"%s expects (buffer, value)",e->name);
        Expr *dst=e->args[0]; const char *bufname=NULL;
        if(dst->kind==E_INDEX&&dst->operand->kind==E_IDENT) bufname=dst->operand->name;
        else if(dst->kind==E_IDENT) bufname=dst->name;
        else die(0,"Interlocked target must be a buffer element");
        const char *m = !strcmp(e->name,"InterlockedAdd")?"add":!strcmp(e->name,"InterlockedMin")?"min":
            !strcmp(e->name,"InterlockedMax")?"max":!strcmp(e->name,"InterlockedAnd")?"and":
            !strcmp(e->name,"InterlockedOr")?"or":!strcmp(e->name,"InterlockedXor")?"xor":
            !strcmp(e->name,"InterlockedExchange")?"exchange":!strcmp(e->name,"InterlockedCompareExchange")?"compare_exchange":NULL;
        if(!m) die(0,"unsupported Interlocked op %s",e->name);
        Expr *id=E(E_IDENT,dst->line,dst->col); id->name=strdup(bufname);
        Expr *der=E(E_DEREF,dst->line,dst->col); der->operand=id;
        Expr *callee=E(E_FIELD,e->line,e->col); callee->operand=der; callee->field=strdup(m);
        Expr *n=E(E_CALL,e->line,e->col); n->callee=callee; n->name=strdup(m);
        n->nargs=e->nargs-1;
        if(n->nargs){ n->args=calloc(n->nargs,sizeof(Expr*));
            for(size_t i=1;i<e->nargs;i++) n->args[i-1]=rw_copy(e->args[i]); }
        *e=*n;
        return;
    }
    ax_expr(e->operand); ax_expr(e->lhs); ax_expr(e->rhs);
    if(e->callee) ax_expr(e->callee);
    for(size_t i=0;i<e->nargs;i++) ax_expr(e->args[i]);
}
static void ax_block(Block *b){ for(size_t i=0;i<b->n;i++){
    Stmt *st=&b->stmts[i];
    if(st->expr) ax_expr(st->expr);
    if(st->init) ax_expr(st->init);
    if(st->cond) ax_expr(st->cond);
    if(st->for_incr) ax_expr(st->for_incr);
    if(st->for_init) ax_expr(st->for_init->expr);
    ax_block(&st->then_b); ax_block(&st->else_b);
    for(size_t c=0;c<st->ncases;c++){ ax_expr(st->cases[c].val); ax_block(&st->cases[c].body); }
    ax_block(&st->def_body);
} }
static void cc_expr(Expr *e, char ***calls, size_t *nc);
static void cc_block(Block *b, char ***calls, size_t *nc){
    for(size_t i=0;i<b->n;i++){
        Stmt *st=&b->stmts[i];
        if(st->expr) cc_expr(st->expr,calls,nc);
        if(st->init) cc_expr(st->init,calls,nc);
        if(st->cond) cc_expr(st->cond,calls,nc);
        if(st->for_incr) cc_expr(st->for_incr,calls,nc);
        if(st->for_init&&st->for_init->expr) cc_expr(st->for_init->expr,calls,nc);
        cc_block(&st->then_b,calls,nc); cc_block(&st->else_b,calls,nc);
        for(size_t c=0;c<st->ncases;c++){ cc_expr(st->cases[c].val,calls,nc); cc_block(&st->cases[c].body,calls,nc); }
        cc_block(&st->def_body,calls,nc);
    }
}
/* call-graph edge collector: every function name invoked by an expression */
static void cc_expr(Expr *e, char ***calls, size_t *nc){
    if(!e) return;
    if(e->kind==E_CALL&&e->name){
        int dup=0;
        for(size_t i=0;i<*nc;i++) if(!strcmp((*calls)[i],e->name)){ dup=1; break; }
        if(!dup){
            char **nn=realloc(*calls,(*nc+1)*sizeof(char*));
            if(nn){ *calls=nn; (*calls)[(*nc)++]=strdup(e->name); }
        }
    }
    cc_expr(e->operand,calls,nc); cc_expr(e->lhs,calls,nc); cc_expr(e->rhs,calls,nc);
    if(e->callee) cc_expr(e->callee,calls,nc);
    for(size_t i=0;i<e->nargs;i++) cc_expr(e->args[i],calls,nc);
}
static void ra_expr(Expr *e, const char *const *res, size_t nres);
static void ra_block(Block *b, const char *const *res, size_t nres){
    for(size_t i=0;i<b->n;i++){
        Stmt *st=&b->stmts[i];
        if(st->expr) ra_expr(st->expr,res,nres);
        if(st->init) ra_expr(st->init,res,nres);
        if(st->cond) ra_expr(st->cond,res,nres);
        if(st->for_incr) ra_expr(st->for_incr,res,nres);
        if(st->for_init&&st->for_init->expr) ra_expr(st->for_init->expr,res,nres);
        ra_block(&st->then_b,res,nres); ra_block(&st->else_b,res,nres);
        for(size_t c=0;c<st->ncases;c++){ ra_expr(st->cases[c].val,res,nres); ra_block(&st->cases[c].body,res,nres); }
        ra_block(&st->def_body,res,nres);
    }
}
/* does an HLSL body/expr tree reference the name? (selective resource capture:
 * a helper gets a forwarded resource param only if its body uses it) */
static int refs_expr(Expr *e, const char *name){
    if(!e) return 0;
    if(e->kind==E_IDENT&&e->name&&!strcmp(e->name,name)) return 1;
    if(e->kind==E_IDENT&&e->name){
        /* a bare cbuffer-field reference (HLSL cbuffer members are module
         * globals): the access requires the cbuffer's resource param */
        char tag[64]; snprintf(tag,sizeof tag,"%s$cb",name);
        for(size_t i=0;i<g_prog.nstructs;i++)
            if(!strcmp(g_prog.structs[i].tag,tag))
                for(size_t f=0;f<g_prog.structs[i].nfields;f++)
                    if(!strcmp(g_prog.structs[i].fields[f].name,e->name)) return 1;
    }
    if(e->kind==E_CALL&&e->name&&!e->callee){
        /* a call forwards the callee's resource params, so the caller must
         * carry them too (transitive resource requirement) */
        int callee_u=0;
        for(size_t i=0;i<g_prog.nfuncs;i++) if(!strcmp(g_prog.funcs[i].name,e->name)){
            for(size_t q=0;q<g_prog.funcs[i].nparams;q++){
                if(!strcmp(g_prog.funcs[i].params[q].name,name)) return 1;
                if(!strcmp(g_prog.funcs[i].params[q].name,"__uniforms")) callee_u=1;
            }
        }
        if(callee_u&&!strcmp(name,"__uniforms")) return 1; /* any callee carrying __uniforms forces the caller to carry it (transitive) */
    }
    if(refs_expr(e->operand,name)||refs_expr(e->lhs,name)||refs_expr(e->rhs,name)) return 1;
    if(e->callee&&refs_expr(e->callee,name)) return 1;
    for(size_t i=0;i<e->nargs;i++) if(refs_expr(e->args[i],name)) return 1;
    return 0;
}
static int refs_block(Block *b, const char *name){
    for(size_t i=0;i<b->n;i++){
        Stmt *st=&b->stmts[i];
        if(st->expr&&refs_expr(st->expr,name)) return 1;
        if(st->init&&refs_expr(st->init,name)) return 1;
        if(st->cond&&refs_expr(st->cond,name)) return 1;
        if(st->for_incr&&refs_expr(st->for_incr,name)) return 1;
        if(st->for_init&&st->for_init->expr&&refs_expr(st->for_init->expr,name)) return 1;
        if(refs_block(&st->then_b,name)||refs_block(&st->else_b,name)) return 1;
        for(size_t c=0;c<st->ncases;c++){ if(refs_expr(st->cases[c].val,name)) return 1; if(refs_block(&st->cases[c].body,name)) return 1; }
        if(refs_block(&st->def_body,name)) return 1;
    }
    return 0;
}
/* resource-arg capture: every call to a lowered user function gets the module
 * resources appended as arguments (the callee carries them as trailing params;
 * the caller's own resource params satisfy the bindings by name) */
static void ra_expr(Expr *e, const char *const *res, size_t nres){
    if(!e) return;
    if(e->kind==E_CALL&&e->name&&!e->callee){
        for(size_t i=0;i<g_prog.nfuncs;i++)
            if(!strcmp(g_prog.funcs[i].name,e->name)){
                if(getenv("BINC_DEBUG_IW")){ fprintf(stderr,"DBG ra: %s callee_nparams=%zu nres=%zu params=[",e->name,g_prog.funcs[i].nparams,nres);
                    for(size_t q2=0;q2<g_prog.funcs[i].nparams;q2++) fprintf(stderr,"%s%s",q2?",":"",g_prog.funcs[i].params[q2].name); fprintf(stderr,"]\n"); }
                /* forward only the resources the callee actually carries
                 * (the callee appends only the ones its body references) */
                for(size_t q=0;q<g_prog.funcs[i].nparams;q++){
                    int isres=0;
                    for(size_t r=0;r<nres;r++) if(!strcmp(g_prog.funcs[i].params[q].name,res[r])){ isres=1; break; }
                    if(!isres) continue;
                    e->args=realloc(e->args,(e->nargs+1)*sizeof(Expr*));
                    Expr *a=E(E_IDENT,e->line,e->col); a->name=strdup(g_prog.funcs[i].params[q].name);
                    e->args[e->nargs++]=a;
                }
                if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG ra: %s nargs=%zu\n",e->name,e->nargs);
                break;
            }
    }
    ra_expr(e->operand,res,nres); ra_expr(e->lhs,res,nres); ra_expr(e->rhs,res,nres);
    if(e->callee) ra_expr(e->callee,res,nres);
    for(size_t i=0;i<e->nargs;i++) ra_expr(e->args[i],res,nres);
}
static void iw_stmt(Stmt *st, const char *fnname, int io){
    switch(st->kind){
    case S_EXPR: iw_expr(st->expr,fnname,io); break;
    case S_DECL: iw_expr(st->init,fnname,io); break;
    case S_RETURN: iw_expr(st->expr,fnname,io); break;
    case S_IF: iw_expr(st->cond,fnname,io); iw_block(&st->then_b,fnname,io); iw_block(&st->else_b,fnname,io); break;
    case S_WHILE: case S_DOWHILE: iw_expr(st->cond,fnname,io); iw_block(&st->then_b,fnname,io); break;
    case S_FOR:
        if(st->for_init) iw_stmt(st->for_init,fnname,io);
        iw_expr(st->for_cond,fnname,io); iw_expr(st->for_incr,fnname,io); iw_block(&st->then_b,fnname,io); break;
    case S_SWITCH:
        iw_expr(st->sw_cond,fnname,io);
        for(size_t i=0;i<st->ncases;i++){ iw_expr(st->cases[i].val,fnname,io); iw_block(&st->cases[i].body,fnname,io); }
        iw_block(&st->def_body,fnname,io); break;
    case S_BLOCK: iw_block(&st->then_b,fnname,io); break;
    default: break;
    }
}
static void iw_block(Block *b, const char *fnname, int io){
    for(size_t i=0;i<b->n;i++) iw_stmt(&b->stmts[i],fnname,io);
}
static void iw_expr(Expr *e, const char *fnname, int io){
    if(!e) return;
    if(e->kind==E_CALL&&e->name&&!strcmp(e->name,fnname)){
        if((size_t)io>=e->nargs) die(0,"inout call has no argument %d",io);
        /* a = f(..., a, ...) — the receiver is the inout arg (index io), the
         * call keeps every arg */
        Expr *receiver=rw_copy(e->args[io]);
        Expr *as=E(E_ASSIGN,e->line,e->col); as->aop=A_ASSIGN; as->operand=receiver; as->rhs=rw_copy(e);
        if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG iw: call->assign operand=%s kind=%d io=%d\n",receiver->name,receiver->kind,io);
        *e=*as;
        return;
    }
    iw_expr(e->operand,fnname,io); iw_expr(e->lhs,fnname,io); iw_expr(e->rhs,fnname,io);
    if(e->callee) iw_expr(e->callee,fnname,io);
    for(size_t i=0;i<e->nargs;i++) iw_expr(e->args[i],fnname,io);
}
static void prog_add_struct(StructDef sd){
    for(size_t i=0;i<g_prog.nstructs;i++)
        if(!strcmp(g_prog.structs[i].tag,sd.tag)){
            if(g_prog.structs[i].is_template&&!sd.is_template){
                /* the concrete instantiation replaces the template forward def */
                free(g_prog.structs[i].fields);
                g_prog.structs[i]=sd;
            }
            return; /* dedup by tag */
        }
    g_prog.structs=realloc(g_prog.structs,(g_prog.nstructs+1)*sizeof(StructDef));
    g_prog.structs[g_prog.nstructs++]=sd;
}

/* compute: rewrite the SV_DispatchThreadID / SV_GroupThreadID params into a
 * coord3D param and rewrite body references to .global / .local */
static void lower_compute(Function *fn, HLSLFunc *hf){
    const char *names[8]; size_t nn=0;
    const char *sdrops[8]; size_t nsd=0; /* struct params folded into the coord (dropped, NOT rw-rewritten) */
    const char *coord_name=NULL; int coord_kind __attribute__((unused)) =0; /* 1=global, 2=local */
    for(size_t i=0;i<hf->np;i++){
        HLSLParam *p=&hf->params[i];
        if(!p->sem) continue;
        if(sem_eq(p->sem,"SV_DispatchThreadID")){ names[nn++]=p->name; if(!coord_name){coord_name=p->name;coord_kind=1;} }
        else if(sem_eq(p->sem,"SV_GroupThreadID")){ names[nn++]=p->name; if(!coord_name){coord_name=p->name;coord_kind=2;} }
        else if(sem_eq(p->sem,"SV_GroupID")){ names[nn++]=p->name; if(!coord_name){coord_name=p->name;coord_kind=3;} }
        else if(sem_eq(p->sem,"SV_GroupIndex")){ names[nn++]=p->name; if(!coord_name){coord_name=p->name;coord_kind=4;} }
    }
    /* SPIRV-Cross pattern: main(SPIRV_Cross_Input stage_input) — a struct param
     * whose fields carry the thread-id semantics. Rewrite stage_input.<svfield>
     * to the coord reference and fold the param into the coord machinery. */
    for(size_t i=0;i<hf->np;i++){
        HLSLParam *p=&hf->params[i];
        if(p->ty.kind!=T_STRUCT||p->ty.is_ptr) continue;
        StructDef *sd=NULL;
        for(size_t si=0;si<g_prog.nstructs;si++) if(!strcmp(g_prog.structs[si].tag,p->ty.struct_name)){ sd=&g_prog.structs[si]; break; }
        if(!sd) continue;
        for(size_t fi=0;fi<sd->nfields;fi++){
            Field *fd=&sd->fields[fi];
            if(!fd->sem) continue;
            const char *fld=NULL;
            if(sem_eq(fd->sem,"SV_DispatchThreadID")) fld="global";
            else if(sem_eq(fd->sem,"SV_GroupThreadID")) fld="local";
            else if(sem_eq(fd->sem,"SV_GroupID")) fld="group";
            if(!fld) continue;
            if(!coord_name){ coord_name=p->name; coord_kind=1; }
            if(getenv("BINC_DEBUG_D3D9")) fprintf(stderr,"DBG sr: %s.%s -> %s.%s\n",p->name,fd->name,coord_name,fld);
            sr_block(&fn->body,p->name,fd->name,coord_name,fld);
            if(nsd<8) sdrops[nsd++]=p->name; /* dropped from the signature (replaced by the coord) */
            break; /* one SV field per input struct (SPIRV-Cross stage inputs) */
        }
    }
    if(!coord_name) return; /* no thread ids: plain kernel */
    /* derived thread ids, each semantic its own rewrite in terms of the one
     * coord param: global = the dispatch thread id; local = global % numthreads;
     * group = global / numthreads; index = linearized local */
    Expr *glob=E(E_FIELD,0,0); { Expr *b=E(E_IDENT,0,0); b->name=strdup(coord_name); glob->operand=b; glob->field=strdup("global"); }
    Expr *local=E(E_FIELD,0,0); { Expr *b=E(E_IDENT,0,0); b->name=strdup(coord_name); local->operand=b; local->field=strdup("local"); }
    for(size_t k=0;k<nn;k++){
        Expr *der=NULL; const char *fld="global";
        for(size_t i=0;i<hf->np;i++) if(hf->params[i].name&&!strcmp(hf->params[i].name,names[k])&&hf->params[i].sem){
            if(sem_eq(hf->params[i].sem,"SV_DispatchThreadID")) fld="global";
            else if(sem_eq(hf->params[i].sem,"SV_GroupThreadID")) fld="local";
            else if(sem_eq(hf->params[i].sem,"SV_GroupID")){ /* group = global / numthreads */
                Expr *d=E(E_BIN,0,0); d->bop=B_DIV; d->lhs=rw_copy(glob); Expr *c=E(E_ICONST,0,0); c->ival=hf->numtx?hf->numtx:1; d->rhs=c; der=d;
            } else if(sem_eq(hf->params[i].sem,"SV_GroupIndex")){ /* linearized local */
                int nx=hf->numtx?hf->numtx:1, ny=hf->numty?hf->numty:1;
                Expr *ix=E(E_FIELD,0,0); ix->operand=rw_copy(local); ix->field=strdup("x");
                if(ny>1){ Expr *iy=E(E_FIELD,0,0); iy->operand=rw_copy(local); iy->field=strdup("y");
                    Expr *t=E(E_BIN,0,0); t->bop=B_MUL; t->lhs=iy; Expr *c=E(E_ICONST,0,0); c->ival=nx; t->rhs=c;
                    Expr *s=E(E_BIN,0,0); s->bop=B_ADD; s->lhs=t; s->rhs=ix; ix=s; }
                der=ix;
            }
            break;
        }
        Rewrite rw={names+k,1,fld,coord_name,der};
        rw_block(&fn->body,&rw);
    }
    /* rebuild params from the live list: drop the SV-thread-id params (matched
     * by name), append one coord (1D for numthreads(x,1,1)) */
    Param *np2=calloc(fn->nparams+1,sizeof(Param)); size_t nn2=0;
    for(size_t i=0;i<fn->nparams;i++){
        int is_sv=0;
        for(size_t k=0;k<nn;k++) if(!strcmp(fn->params[i].name,names[k])){ is_sv=1; break; }
        for(size_t k=0;k<nsd;k++) if(!strcmp(fn->params[i].name,sdrops[k])){ is_sv=1; break; }
        if(is_sv) continue;
        np2[nn2++]=fn->params[i];
    }
    Param cp={0};
    cp.name=strdup(coord_name); cp.ty.kind=T_COORD; cp.ty.coordn=3;
    np2[nn2++]=cp;
    free(fn->params); fn->params=np2; fn->nparams=nn2;
}

/* fragment: map stage-in struct fields, synthesize the stage-out struct */
static void lower_fragment(Function *fn, HLSLFunc *hf){
    /* D3D9 direct semantic params (float4 Diffuse : COLOR0, float2 Tex : TEXCOORD0):
     * pack into a synthetic stage-in struct exactly like the vertex path */
    {
        size_t attr_count=0;
        for(size_t i=0;i<hf->np;i++){
            HLSLParam *p=&hf->params[i];
            if(p->ty.is_ptr||p->ty.kind==T_TEXTURE||p->ty.kind==T_SAMPLER||p->ty.kind==T_STRUCT) continue;
            if(p->is_uniform) continue;
            attr_count++;
        }
        if(attr_count){
            char tag[64]; snprintf(tag,sizeof tag,"%s$in",fn->name);
            StructDef sd={0}; sd.tag=strdup(tag);
            sd.fields=calloc(attr_count,sizeof(Field));
            size_t fi=0;
            for(size_t i=0;i<hf->np;i++){
                HLSLParam *p=&hf->params[i];
                if(p->ty.is_ptr||p->ty.kind==T_TEXTURE||p->ty.kind==T_SAMPLER||p->ty.kind==T_STRUCT) continue;
                if(p->is_uniform) continue;
                Field *f=&sd.fields[fi++];
                f->name=strdup(p->name); f->ty=p->ty;
                f->sem=p->sem?strdup(p->sem):NULL;
                if(!p->sem){ f->attr=5; f->attr_idx=16+(int)fi; }
                else sem_to_attr(p->sem,&f->attr,&f->attr_idx,0);
            }
            sd.nfields=fi;
            prog_add_struct(sd);
            const char **names=malloc(attr_count*sizeof(char*));
            size_t nn=0;
            Param *np2=calloc(fn->nparams+1,sizeof(Param)); size_t nn2=0;
            for(size_t i=0;i<fn->nparams;i++){
                Param *p=&fn->params[i];
                int is_attr = !(p->ty.is_ptr||p->ty.kind==T_TEXTURE||p->ty.kind==T_SAMPLER||p->ty.kind==T_STRUCT);
                if(is_attr){ names[nn++]=p->name; continue; }
                np2[nn2++]=(Param){p->name,p->ty,UN_UNIFORM};
            }
            Param in={0}; in.name=strdup("__in"); in.ty.kind=T_STRUCT; in.ty.struct_name=strdup(tag);
            np2[nn2++]=in;
            free(fn->params); fn->params=np2; fn->nparams=nn2;
            for(size_t i=0;i<nn;i++){
                const char *nm=names[i];
                Rewrite rw={&nm,1,nm,"__in",NULL};
                rw_block(&fn->body,&rw);
            }
            free(names);
        }
    }
    for(size_t i=0;i<fn->nparams;i++){
        Param *p=&fn->params[i];
        if(p->ty.kind!=T_STRUCT||p->ty.is_ptr) continue;
        StructDef *sd=NULL;
        for(size_t s=0;s<g_prog.nstructs;s++) if(!strcmp(g_prog.structs[s].tag,p->ty.struct_name)){ sd=&g_prog.structs[s]; break; }
        if(!sd) continue;
        for(size_t f=0;f<sd->nfields;f++) sem_to_attr(sd->fields[f].sem,&sd->fields[f].attr,&sd->fields[f].attr_idx,0);
    }
    if(hf->ret_sem&&hf->ret.kind!=T_VOID&&hf->ret.kind!=T_STRUCT){
        /* scalar/vector return with a semantic: synthesize a stage-out struct.
         * struct returns carry per-field semantics already and pass through. */
        char tag[64]; snprintf(tag,sizeof tag,"%s$out",fn->name);
        StructDef sd={0}; sd.tag=strdup(tag);
        Field *f=calloc(1,sizeof(Field));
        if(sem_pref(hf->ret_sem,"SV_Target")){
            int n=atoi(hf->ret_sem+9);
            char nm[24]; snprintf(nm,sizeof nm,"color%d",n);
            f->name=strdup(nm); f->attr=3; f->attr_idx=n;
        } else if(sem_eq(hf->ret_sem,"SV_Depth")){
            f->name=strdup("depth"); f->attr=4;
        } else {
            f->name=strdup("color0"); f->attr=3; f->attr_idx=0;
        }
        f->ty=hf->ret;
        sd.fields=f; sd.nfields=1;
        prog_add_struct(sd);
        /* prepend the out-struct local and rewrite returns */
        Stmt decl={0}; decl.kind=S_DECL; decl.line=hf->line; decl.col=0;
        decl.ty.kind=T_STRUCT; decl.ty.struct_name=strdup(tag);
        decl.name=strdup("__out");
        Block nb={0};
        nb.stmts=malloc(sizeof(Stmt)); nb.n=1; nb.stmts[0]=decl;
        nb.stmts=realloc(nb.stmts,(fn->body.n+1)*sizeof(Stmt));
        memmove(&nb.stmts[1],fn->body.stmts,fn->body.n*sizeof(Stmt));
        nb.n=fn->body.n+1;
        free(fn->body.stmts); fn->body=nb;
        rr_block(&fn->body,"__out",f->name);
        fn->ret.kind=T_STRUCT; fn->ret.struct_name=strdup(tag); fn->ret.vecn=0; fn->ret.matn=0;
    }
    /* struct returns with semantic fields: map the field attrs (SV_Target0..) */
    if(hf->ret.kind==T_STRUCT){
        StructDef *sd=NULL;
        for(size_t s=0;s<g_prog.nstructs;s++) if(!strcmp(g_prog.structs[s].tag,hf->ret.struct_name)){ sd=&g_prog.structs[s]; break; }
        if(sd) for(size_t f=0;f<sd->nfields;f++){
            if(!sd->fields[f].sem) continue;
            if(sem_pref(sd->fields[f].sem,"COLOR")){ /* D3D9 PS out: COLORn == render target n */
                sd->fields[f].attr=3; sd->fields[f].attr_idx=atoi(sd->fields[f].sem+5); continue;
            }
            if(sem_pref(sd->fields[f].sem,"TEXCOORD")){ /* D3D9 PS out: TEXCOORDn == render target n */
                sd->fields[f].attr=3; sd->fields[f].attr_idx=atoi(sd->fields[f].sem+8); continue;
            }
            sem_to_attr(sd->fields[f].sem,&sd->fields[f].attr,&sd->fields[f].attr_idx,0);
        }
    }
}

/* vertex: attribute params -> synthetic stage-in struct; ret struct attrs */
static void lower_vertex(Function *fn, HLSLFunc *hf){
    /* SV_VertexID params use the vertex_id machinery */
    for(size_t i=0;i<hf->np;i++){
        HLSLParam *p=&hf->params[i];
        if(p->sem&&sem_eq(p->sem,"SV_VertexID")){ p->ty.kind=T_UINT32; p->ty.vecn=0; p->ty.as=AS_THREAD; }
    }
    /* user-defined struct params (D3D9 VSInput style) ARE the stage-in struct:
     * unpacked directly by the codegen; set their field attrs here */
    for(size_t i=0;i<hf->np;i++){
        HLSLParam *p=&hf->params[i];
        if(p->ty.kind!=T_STRUCT||p->ty.is_ptr) continue;
        StructDef *sd=NULL;
        for(size_t s=0;s<g_prog.nstructs;s++) if(!strcmp(g_prog.structs[s].tag,p->ty.struct_name)){ sd=&g_prog.structs[s]; break; }
        if(sd) for(size_t f=0;f<sd->nfields;f++) sem_to_attr(sd->fields[f].sem,&sd->fields[f].attr,&sd->fields[f].attr_idx,1);
    }
    /* ret struct: map semantics to attrs (must run even when there are no
     * scalar/vector attrs — an all-struct-input VS returns early below) */
    if(fn->ret.kind==T_STRUCT){
        for(size_t s=0;s<g_prog.nstructs;s++) if(!strcmp(g_prog.structs[s].tag,fn->ret.struct_name)){
            StructDef *sd=&g_prog.structs[s];
            for(size_t f=0;f<sd->nfields;f++) sem_to_attr(sd->fields[f].sem,&sd->fields[f].attr,&sd->fields[f].attr_idx,0);
        }
    }
    /* gather attribute params (non-pointer, non-coord scalar/vector types) */
    size_t attr_count=0;
    for(size_t i=0;i<hf->np;i++){
        HLSLParam *p=&hf->params[i];
        if(p->ty.is_ptr||p->ty.kind==T_TEXTURE||p->ty.kind==T_SAMPLER) continue;
        if(p->ty.kind==T_STRUCT) continue; /* unpacked directly */
        if(p->is_uniform) continue; /* per-draw constants live in __uniforms */
        if(p->sem&&sem_eq(p->sem,"SV_VertexID")) continue;
        attr_count++;
    }
    if(!attr_count) return;
    /* build the stage-in struct */
    char tag[64]; snprintf(tag,sizeof tag,"%s$in",fn->name);
    StructDef sd={0}; sd.tag=strdup(tag);
    sd.fields=calloc(attr_count,sizeof(Field));
    size_t fi=0;
    for(size_t i=0;i<hf->np;i++){
        HLSLParam *p=&hf->params[i];
        if(p->ty.is_ptr||p->ty.kind==T_TEXTURE||p->ty.kind==T_SAMPLER) continue;
        if(p->ty.kind==T_STRUCT) continue;
        if(p->is_uniform) continue;
        if(p->sem&&sem_eq(p->sem,"SV_VertexID")) continue;
        Field *f=&sd.fields[fi++];
        f->name=strdup(p->name); f->ty=p->ty;
        f->sem=p->sem?strdup(p->sem):NULL;
        if(!p->sem){ f->attr=5; f->attr_idx=16+(int)fi; } /* semantic-less: keep out of the D3D9 locn table */
        else sem_to_attr(p->sem,&f->attr,&f->attr_idx,1);
    }
    sd.nfields=fi;
    prog_add_struct(sd);
    /* replace the attribute params with one stage-in struct param; keep the
     * resource params (cbuffers, buffers, textures, samplers) in place */
    const char **names=malloc(attr_count*sizeof(char*));
    size_t nn=0;
    Param *np2=calloc(fn->nparams+1,sizeof(Param)); size_t nn2=0;
    for(size_t i=0;i<fn->nparams;i++){
        Param *p=&fn->params[i];
        int is_svvid=0;
        for(size_t j=0;j<hf->np;j++)
            if(!strcmp(hf->params[j].name,p->name)&&hf->params[j].sem&&sem_eq(hf->params[j].sem,"SV_VertexID")) is_svvid=1;
        if(is_svvid){ /* vertex_id built-in: keep as a thread-typed param */
            p->ty.kind=T_UINT32; p->ty.vecn=0; p->ty.as=AS_THREAD;
            np2[nn2++]=(Param){p->name,p->ty,UN_UNIFORM}; continue; }
        int is_attr = !(p->ty.is_ptr||p->ty.kind==T_TEXTURE||p->ty.kind==T_SAMPLER||p->ty.kind==T_STRUCT);
        if(is_attr){ names[nn++]=p->name; continue; }
        np2[nn2++]=(Param){p->name,p->ty,UN_UNIFORM};
    }
    Param in={0}; in.name=strdup("__in"); in.ty.kind=T_STRUCT; in.ty.struct_name=strdup(tag);
    np2[nn2++]=in;
    free(fn->params); fn->params=np2; fn->nparams=nn2;
    /* rewrite body references: E_IDENT(pname) -> E_FIELD(E_IDENT(__in), pname) */
    for(size_t i=0;i<nn;i++){
        const char *nm=names[i];
        Rewrite rw={&nm,1,nm,"__in",NULL};
        rw_block(&fn->body,&rw);
    }
    free(names);
}

/* hlsl_build: lower an HLSLProg onto the shared Program AST.
 * entry/profile select the compute/vertex/fragment entry; with stage_all,
 * every function whose return semantics imply a stage gets lowered (render
 * files with multiple stage functions, e.g. shaders.hlsl VS+PS pairs). */
static char **iw_helpers=NULL; static size_t niw=0; static int *iw_io=NULL;
Program hlsl_build(HLSLProg *hp, const char *entry, const char *profile, int stage_all){
    memset(&g_prog,0,sizeof g_prog);
    g_prog.hlsl=1;
    fprintf(stderr,"DBG build: entry=%s nglobals=%zu\n",entry,hp->nglobals);
    int is_vs = strncmp(profile,"vs",2)==0;
    int is_ps = strncmp(profile,"ps",2)==0;
    int is_gs = strncmp(profile,"gs",2)==0;
    /* module constants: static const globals -> ConstDef; mutable static
     * scalars (SPIRV-Cross `static uint3 gl_GlobalInvocationID;` pattern)
     * become mutable module globals (mut=1) */
    for(size_t i=0;i<hp->nglobals;i++){
        HLSLGlobal *gg=&hp->globals[i];
        if(gg->is_groupshared) continue;
        if(gg->ty.is_ptr||gg->ty.kind==T_TEXTURE||gg->ty.kind==T_SAMPLER) continue; /* resources */
        if(!gg->is_const&&!gg->has_init&&!gg->is_static) continue; /* D3D9 uniforms fold into __uniforms */
        ConstDef cd={0};
        cd.name=strdup(gg->name); cd.ty=gg->ty; cd.line=gg->line;
        cd.is_int=gg->is_int; cd.ival=gg->ival; cd.fval=gg->fval;
        cd.mut = !gg->is_const; /* mutable: written by functions */
        g_prog.consts=realloc(g_prog.consts,(g_prog.nconsts+1)*sizeof(ConstDef));
        g_prog.consts[g_prog.nconsts++]=cd;
    }
    /* structs */
    for(size_t i=0;i<hp->nstructs;i++){
        HLSLStruct *hs=&hp->structs[i];
        StructDef sd={0}; sd.tag=strdup(hs->tag);
        sd.fields=hs->fields; sd.nfields=hs->nfields;
        prog_add_struct(sd);
    }
    /* cbuffers -> constant-buffer structs: <cbname>$cb, with the D3D packing.
     * HLSL packs members into 16-byte registers: a member starts at the next
     * offset aligned to 4 (vectors pack tightly) that does not cross a register
     * boundary; vec4 stays 16-aligned. The natural MSL/AIR layout only differs
     * where a tight-packed vec2/vec3 would land 8/16-aligned, so those fields
     * become packed arrays ([2 x float] = packed_float2, align 4) and pad
     * fields are injected when a member would cross a register. */
    for(size_t i=0;i<hp->ncbufs;i++){
        HLCBuf *cb=&hp->cbufs[i];
        char tag[64]; snprintf(tag,sizeof tag,"%s$cb",cb->name);
        Field *f=calloc(cb->nfields+16,sizeof(Field));
        size_t nf=0, d3d=0, emit=0, npad=0;
        for(size_t j=0;j<cb->nfields;j++){
            Field *sf=&cb->fields[j];
            int sz = (sf->ty.vecn==4?16:sf->ty.vecn==3?12:sf->ty.vecn==2?8:4) * (sf->ty.array_n?sf->ty.array_n*(sf->ty.array_m?sf->ty.array_m:1):1);
            int d3d_off=(int)d3d;
            if((d3d_off%16)+sz>16) d3d_off=((d3d_off/16)+1)*16; /* register boundary */
            while(emit<(size_t)d3d_off){ /* explicit padding to the D3D offset */
                char pn[32]; snprintf(pn,sizeof pn,"__pad%zu",npad++);
                Type pt={0}; pt.kind=T_FLOAT;
                f[nf++]=(Field){strdup(pn),pt,0,0,NULL};
                emit+=4;
            }
            Type ft=sf->ty;
            if(!ft.array_n&&!ft.array_m&&(ft.vecn==2||ft.vecn==3)){ ft.array_n=ft.vecn; ft.vecn=0; } /* packed vector (not an array field — arrays keep their element vector type) */
            f[nf++]=(Field){strdup(sf->name),ft,0,0,sf->sem?strdup(sf->sem):NULL};
            emit+=sz; d3d=(size_t)d3d_off+sz;
        }
        StructDef sd={0}; sd.tag=strdup(tag);
        sd.fields=f; sd.nfields=nf;
        prog_add_struct(sd);
    }
    /* resource table: cbuffers + typed globals, ordered by register slot
     * (matches the DXC/SPIRV-Cross buffer index ordering for the diff) */
    typedef struct { const char *name; Type ty; int reg; } HRes;
    HRes res[64]; size_t nres=0;
    for(size_t i=0;i<hp->ncbufs;i++){
        HLCBuf *cb=&hp->cbufs[i];
        Type t={0}; t.kind=T_STRUCT;
        char tag[64]; snprintf(tag,sizeof tag,"%s$cb",cb->name);
        t.struct_name=strdup(tag); t.is_ptr=1; t.as=AS_CONSTANT;
        res[nres++]=(HRes){cb->name,t,cb->reg>=0?cb->reg:0};
    }
    for(size_t i=0;i<hp->nglobals;i++){
        HLSLGlobal *gg=&hp->globals[i];
        if(gg->is_const) continue;
        if(gg->is_groupshared){ Type t=gg->ty; t.as=AS_THREADGROUP; res[nres++]=(HRes){gg->name,t,0}; continue; }
        if(!(gg->ty.is_ptr||gg->ty.kind==T_TEXTURE||gg->ty.kind==T_SAMPLER)) continue;
        Type rt=gg->ty;
        /* RWStructuredBuffer<uint|int> -> atomic buffers (the Interlocked ops) */
        if(gg->ty.is_ptr&&!gg->ty.struct_name&&(gg->ty.kind==T_UINT32||gg->ty.kind==T_INT32)){
            Type at={0}; at.kind=T_ATOMIC; at.atomic_base=gg->ty.kind; at.is_ptr=1; at.as=gg->ty.as;
            rt=at;
        }
        res[nres++]=(HRes){gg->name,rt,gg->reg>=0?gg->reg:0};
    }
    HLSLResourceField *resource_fields=NULL; size_t nresource_fields=0;
    for(size_t fi=0;fi<hp->nfuncs;fi++)
        resource_field_collect_block(&hp->funcs[fi].body,hp,&resource_fields,&nresource_fields);
    int next_resource_reg=0;
    for(size_t ri=0;ri<nres;ri++) if(res[ri].reg>=next_resource_reg) next_resource_reg=res[ri].reg+1;
    for(size_t ri=0;ri<nresource_fields&&nres<64;ri++){
        int duplicate=0;
        for(size_t q=0;q<nres;q++) if(!strcmp(res[q].name,resource_fields[ri].flat)){ duplicate=1; break; }
        if(!duplicate) res[nres++]=(HRes){resource_fields[ri].flat,resource_fields[ri].ty,next_resource_reg++};
    }
    for(size_t fi=0;fi<hp->nfuncs;fi++)
        resource_field_rewrite_block(&hp->funcs[fi].body,resource_fields,nresource_fields);
    Rewrite unif_rw[64]; size_t nu=0; /* D3D9 uniform rewrites (built below) */
    /* D3D9 uniforms: non-const scalar/vector globals pack into one const
     * struct (like a cbuffer); the harness binds it at the register index
     * that follows the declared resources. */
    {
        size_t nuf=0; Field uf[64]; int ureg=1000;
        for(size_t i=0;i<hp->nglobals;i++){
            HLSLGlobal *gg=&hp->globals[i];
            if(getenv("BINC_DEBUG_D3D9")) fprintf(stderr,"DBG unif: %s kind=%d isc=%d vecn=%d an=%d\n",gg->name,gg->ty.kind,gg->is_const,gg->ty.vecn,gg->ty.array_n);
            if(gg->is_const||gg->is_groupshared||gg->is_static) continue; /* const/groupshared/static stay out of __uniforms */
            if(gg->ty.is_ptr||gg->ty.kind==T_TEXTURE||gg->ty.kind==T_SAMPLER) continue;
            if(gg->ty.kind!=T_FLOAT&&gg->ty.kind!=T_INT32&&gg->ty.kind!=T_UINT32&&gg->ty.kind!=T_BOOL&&gg->ty.kind!=T_HALF&&gg->ty.kind!=T_STRUCT) continue;
            Type ft=gg->ty;
            /* D3D9 constant-register alignment: float2/float3 uniforms occupy a
             * full float4 register (elements of floatN arrays align to 16 bytes).
             * The consumer truncates <4 x float> back to float2/float3. */
            if((ft.vecn==2||ft.vecn==3)&&ft.kind!=T_STRUCT) ft.vecn=4;
            uf[nuf++]=(Field){strdup(gg->name),ft,0,0,NULL};
        }
        /* `uniform` params (D3D9 per-draw constants, e.g. `uniform bool
         * bTexture`) pack into the same const struct; their references
         * rewrite to __uniforms.<name> exactly like uniform globals. */
        for(size_t i=0;i<hp->nfuncs;i++){
            HLSLFunc *hf=&hp->funcs[i];
            for(size_t p=0;p<hf->np;p++){
                HLSLParam *hp2=&hf->params[p];
                if(!hp2->is_uniform) continue;
                size_t dup=0; for(size_t d=0;d<nuf;d++) if(!strcmp(uf[d].name,hp2->name)){ dup=1; break; }
                if(dup) continue;
                Type ft=hp2->ty;
                if(ft.vecn==2||ft.vecn==3) ft.vecn=4;
                uf[nuf++]=(Field){strdup(hp2->name),ft,0,0,NULL};
            }
        }
        if(nuf){
            char tag[64]; snprintf(tag,sizeof tag,"$uniforms");
            Field *ufh=calloc(nuf,sizeof(Field));
            for(size_t di=0;di<nuf;di++) ufh[di]=uf[di];
            StructDef sd={0}; sd.tag=strdup(tag); sd.fields=ufh; sd.nfields=nuf;
            prog_add_struct(sd);
            Type ut={0}; ut.kind=T_STRUCT; ut.struct_name=strdup(tag); ut.is_ptr=1; ut.as=AS_CONSTANT;
            res[nres++]=(HRes){"__uniforms",ut,ureg};
            for(size_t fi=0;fi<nuf;fi++){ /* the fn loop applies these below */
                const char **nmarr=malloc(sizeof(char*)); nmarr[0]=ufh[fi].name;
                Rewrite rw={nmarr,1,ufh[fi].name,"__uniforms",NULL};
                if(nu<64) unif_rw[nu++]=rw;
            }
        }
    }
    /* insertion sort by register slot */
    for(size_t i=1;i<nres;i++){ HRes t=res[i]; size_t j=i;
        while(j>0&&res[j-1].reg>t.reg){ res[j]=res[j-1]; j--; } res[j]=t; }
    if(getenv("BINC_DEBUG_IW")){ fprintf(stderr,"DBG res: nres=%zu [",nres); for(size_t i=0;i<nres;i++) fprintf(stderr,"%s%s",i?",":"",res[i].name); fprintf(stderr,"]\n"); }
    /* functions: all of them, plain; the entry gets its stage */
    int entry_found=0;
    /* reachability (single-entry mode): only functions reachable from the
     * entry are lowered/emitted. UE files carry thousands of dead helpers
     * (DFMath, LTC, noise) whose unsupported constructs would otherwise fail
     * codegen for every entry. stage_all keeps the historical full emit. */
    int *reach=NULL;
    if(!stage_all){
        reach=calloc(hp->nfuncs,sizeof(int));
        size_t *q=calloc(hp->nfuncs?hp->nfuncs:1,sizeof(size_t));
        size_t head=0,tail=0;
        for(size_t i=0;i<hp->nfuncs;i++) if(!strcmp(hp->funcs[i].name,entry)){ reach[i]=1; q[tail++]=i; }
        while(head<tail){
            size_t fi=q[head++];
            char **calls=NULL; size_t nc=0;
            cc_block(&hp->funcs[fi].body,&calls,&nc);
            for(size_t c=0;c<nc;c++){
                for(size_t j=0;j<hp->nfuncs;j++)
                    if(!reach[j]&&!strcmp(hp->funcs[j].name,calls[c])){ reach[j]=1; q[tail++]=j; }
                free(calls[c]);
            }
            free(calls);
        }
        free(q);
    }
    for(size_t i=0;i<hp->nfuncs;i++){
        HLSLFunc *hf=&hp->funcs[i];
        if(reach&&!reach[i]) continue; /* dead helper: skip entirely */
        Function fn={0};
        fn.name=strdup(hf->name); fn.line=hf->line;
        fn.ret=hf->ret; fn.body=hf->body;
        rn_block(&fn.body); /* HLSL barriers -> sync */
        ax_block(&fn.body); /* Interlocked* -> the atomic method form */
        fn.params=calloc(hf->np?hf->np:1,sizeof(Param));
        fn.nparams=0;
        for(size_t p=0;p<hf->np;p++){
            HLSLParam *hp2=&hf->params[p];
            fn.params[fn.nparams++]=(Param){strdup(hp2->name),hp2->ty,UN_UNIFORM,hp2->def};
        }
        /* inout/out helpers (Phase 5): single inout on a void function becomes a
         * value return; the call sites are rewritten to `a = f(b, ...)` */
        {
            int io=-1;
            int skip=0;
            for(size_t p=0;p<hf->np;p++) if(hf->params[p].inq==2||hf->params[p].inq==3){
                if(io>=0){ /* multiple out/inout params: no Metal value-return
                            * translation yet — emit as plain value params with a
                            * warning (semantics wrong if actually called) */
                    fprintf(stderr,"binc: warning: %s: multiple out/inout params not supported (values won't propagate) — line %d\n",hf->name,hf->line);
                    skip=1; break;
                }
                io=(int)p;
            }
            if(io>=0&&hf->ret.kind!=T_VOID){ /* value-returning fn with an out param
                                              * would need a struct return — skip */
                fprintf(stderr,"binc: warning: %s: out param on a value-returning function not supported (value won't propagate) — line %d\n",hf->name,hf->line);
                skip=1;
            }
            if(skip){ /* reset to plain value params so the signature is at least emit-table */
                for(size_t p=0;p<hf->np;p++) hf->params[p].inq=0;
                io=-1;
            }
            if(io>=0){
                if(hf->ret.kind!=T_VOID) die(hf->line,"inout param on a value-returning function not supported yet");
                fn.ret=hf->params[io].ty;
                /* params are read-only: copy the inout param into a mutable local
                 * (`float3 ai = ai$in;` for inout, a bare decl for out) */
                char inname[64]; snprintf(inname,sizeof inname,"%s$in",hf->params[io].name);
                for(size_t p=0;p<fn.nparams;p++) if(!strcmp(fn.params[p].name,hf->params[io].name))
                    fn.params[p].name=strdup(inname);
                Stmt decl={0}; decl.kind=S_DECL; decl.line=fn.line;
                decl.ty=hf->params[io].ty; decl.name=strdup(hf->params[io].name);
                if(hf->params[io].inq==3){ decl.init=E(E_IDENT,fn.line,0); decl.init->name=strdup(inname); }
                fn.body.stmts=realloc(fn.body.stmts,(fn.body.n+1)*sizeof(Stmt));
                memmove(&fn.body.stmts[1],&fn.body.stmts[0],fn.body.n*sizeof(Stmt));
                fn.body.stmts[0]=decl; fn.body.n++;
                /* append `return <name>;` */
                Stmt ret={0}; ret.kind=S_RETURN; ret.expr=E(E_IDENT,fn.line,0);
                ret.expr->name=strdup(hf->params[io].name);
                fn.body.stmts=realloc(fn.body.stmts,(fn.body.n+1)*sizeof(Stmt));
                fn.body.stmts[fn.body.n++]=ret;
                /* fn.body initially aliases hf->body.  The inout rewrite may
                 * move the statement array, so publish the new owner before
                 * the later resource-reference scan walks hf->body. */
                hf->body=fn.body;
                /* remember the helper so every function's call sites get rewritten */
                iw_helpers=realloc(iw_helpers,(niw+1)*sizeof(char*));
                iw_helpers[niw]=strdup(hf->name);
                iw_io=realloc(iw_io,(niw+1)*sizeof(int));
                iw_io[niw]=(int)io; /* the inout param index (not always arg0) */
                niw++;
            }
        }
        int is_entry = !strcmp(hf->name,entry);
        if(getenv("BINC_DEBUG_D3D9")) fprintf(stderr,"DBG fn: %s is_entry=%d np=%zu\n",hf->name,is_entry,hf->np);
        int s_vs=0, s_ps=0;
        if(stage_all){
            /* infer the stage from the return semantics: SV_Target -> fragment;
             * a struct return carrying SV_Position (or the POSITION semantic)
             * -> vertex (semantics are case-insensitive) */
            if(hf->ret_sem&&(sem_pref(hf->ret_sem,"SV_Target")||sem_pref(hf->ret_sem,"COLOR"))) s_ps=1;
            else if(hf->ret_sem&&(sem_pref(hf->ret_sem,"SV_Position")||sem_pref(hf->ret_sem,"POSITION"))) s_vs=1;
            else if(hf->ret.kind==T_STRUCT){
                StructDef *sd=NULL;
                for(size_t si=0;si<g_prog.nstructs;si++)
                    if(!strcmp(g_prog.structs[si].tag,hf->ret.struct_name)){ sd=&g_prog.structs[si]; break; }
                if(sd) for(size_t fi=0;fi<sd->nfields;fi++){
                    Field *fd=&sd->fields[fi];
                    if(fd->sem&&(sem_pref(fd->sem,"SV_Position")||sem_pref(fd->sem,"POSITION"))) s_vs=1;
                    if(fd->sem&&(sem_pref(fd->sem,"SV_Target")||sem_pref(fd->sem,"COLOR"))) s_ps=1;
                }
            }
        }
        if(is_entry||(stage_all&&(s_vs||s_ps||is_gs))){            if(is_entry) entry_found=1;
            if(stage_all){ fn.stage=is_entry&&is_gs?ST_GEOMETRY:(s_vs?ST_VERTEX:s_ps?ST_FRAGMENT:is_gs?ST_GEOMETRY:ST_FRAGMENT); }
            else if(is_entry){ fn.stage=is_vs?ST_VERTEX:is_ps?ST_FRAGMENT:is_gs?ST_GEOMETRY:ST_NONE; }
        }
        /* D3D9 tex2D family -> the method form BEFORE the resource scan, so a
         * texture only referenced through tex2D(s, uv) is still forwarded */
        tex2d_block(&fn.body,hp);
        /* resources + rewrites for EVERY reachable function: helpers reference
         * module globals and cbuffer fields directly (D3D12 sample pattern —
         * `g_SortBuffer.Load()` inside LoadKeyIndexPair, `Constants.x`, ...).
         * Only the resources the body actually references become trailing
         * params — otherwise every helper would carry the whole module. */
        for(size_t r=0;r<nres;r++){
            /* __uniforms is rewritten into the body by field name, so the
             * literal never appears — forward it only when the body actually
             * references a uniform global/param (pure helpers stay clean) */
            if(!strcmp(res[r].name,"__uniforms")){
                int unif_ref=0;
                for(size_t ui2=0;ui2<nu;ui2++) if(refs_block(&hf->body,unif_rw[ui2].names[0])){ unif_ref=1; break; }
                if(!unif_ref&&refs_block(&hf->body,"__uniforms")) unif_ref=1; /* transitive: callee carries it */
                if(!unif_ref) continue;
            } else if(!refs_block(&hf->body,res[r].name)){
                if(getenv("BINC_DEBUG_IW")&&!strcmp(res[r].name,"Primitive")) fprintf(stderr,"DBG noref: %s nstmts=%zu\n",hf->name,hf->body.n);
                continue;
            }
            if(getenv("BINC_DEBUG_IW")&&!strcmp(res[r].name,"Primitive")) fprintf(stderr,"DBG ref: %s nstmts=%zu\n",hf->name,hf->body.n);
            /* insert the resource param BEFORE any trailing defaulted params:
             * HLSL defaults are trailing, and the RA-appended resource args
             * fill the non-defaulted tail — resources must precede defaults */
            size_t ip=fn.nparams;
            while(ip>0&&fn.params[ip-1].def) ip--;
            fn.params=realloc(fn.params,(fn.nparams+1)*sizeof(Param));
            memmove(&fn.params[ip+1],&fn.params[ip],(fn.nparams-ip)*sizeof(Param));
            fn.params[ip]=(Param){strdup(res[r].name),res[r].ty,UN_UNIFORM};
            fn.nparams++;
        }
        {
            size_t keep=0;
            for(size_t q=0;q<fn.nparams;q++){
                int unif=0;
                for(size_t p=0;p<hf->np;p++)
                    if(!strcmp(hf->params[p].name,fn.params[q].name)&&hf->params[p].is_uniform){ unif=1; break; }
                if(!unif) fn.params[keep++]=fn.params[q];
            }
            fn.nparams=keep;
        }
        for(size_t ci=0;ci<hp->ncbufs;ci++){
            HLCBuf *cb=&hp->cbufs[ci];
            for(size_t fi=0;fi<cb->nfields;fi++){
                const char *fname=cb->fields[fi].name;
                /* a function parameter shadows a same-named cbuffer field
                 * (UE: GetPrimitive_PerObjectGBufferData_FromFlags(uint Flags)
                 * vs cbuffer Primitive{ uint Flags; ... }) — rewriting the
                 * param reference into Primitive.Flags breaks resolution */
                int shadowed=0;
                for(size_t pi=0;pi<hf->np;pi++)
                    if(!strcmp(hf->params[pi].name,fname)){ shadowed=1; break; }
                if(shadowed) continue;
                Rewrite rw={&fname,1,fname,cb->name,NULL};
                rw_block(&fn.body,&rw);
            }
        }
        for(size_t ui=0;ui<nu;ui++){ if(getenv("BINC_DEBUG_IW")&&!strcmp(hf->name,"GetPrimitive_PerObjectGBufferData_FromFlags")) fprintf(stderr,"DBG rw: %s -> %s.%s\n",unif_rw[ui].names[0],unif_rw[ui].base?unif_rw[ui].base:"?",unif_rw[ui].field); rw_block(&fn.body,&unif_rw[ui]); }
        if(is_entry||(stage_all&&(s_vs||s_ps||is_gs))){
            if(fn.stage==ST_VERTEX) lower_vertex(&fn,hf);
            else if(fn.stage==ST_FRAGMENT) lower_fragment(&fn,hf);
            else if(fn.stage==ST_GEOMETRY){
                /* GS lowering to the Metal mesh stage is the in-scope next chunk
                 * (probed: this toolchain dropped air.amplification; the mesh
                 * path exists via metal_mesh). Parse/classification land here. */
                die(hf->line,"geometry shader lowering (Metal mesh) not yet wired — GS parses, stage classified");
            }
            if(is_entry&&!is_vs&&!is_ps&&!is_gs&&!stage_all){
                fn.is_kernel=1;
                if(fn.ret.kind!=T_VOID) die(hf->line,"compute entry must return void");
                lower_compute(&fn,hf);
            }
        }
        g_prog.funcs=realloc(g_prog.funcs,(g_prog.nfuncs+1)*sizeof(Function));
        if(getenv("BINC_DEBUG_IW")&&(!strcmp(hf->name,"GetPrimitive_PerObjectGBufferData_FromFlags")||!strcmp(hf->name,"RenderScenePS"))){ fprintf(stderr,"DBG fnlow: %s nparams=%zu nstmts=%zu",hf->name,fn.nparams,fn.body.n);
            for(size_t qi=0;qi<fn.nparams;qi++) fprintf(stderr," [%s]",fn.params[qi].name); fprintf(stderr,"\n"); }
        g_prog.funcs[g_prog.nfuncs++]=fn;
    }
    if(!entry_found&&!stage_all) die(0,"entry point '%s' not found in the shader",entry);
    free(reach);
    /* capture module resources at every user-function call site (helper bodies
     * reference globals directly; the call must forward them) */
    {
        const char **rn=calloc(nres?nres:1,sizeof(char*));
        for(size_t r=0;r<nres;r++) rn[r]=res[r].name;
        for(size_t i=0;i<g_prog.nfuncs;i++) ra_block(&g_prog.funcs[i].body,rn,nres);
        free(rn);
    }
    /* rewrite every inout-helper call site across all functions */
    for(size_t h=0;h<niw;h++) for(size_t i=0;i<g_prog.nfuncs;i++) iw_block(&g_prog.funcs[i].body,iw_helpers[h],iw_io[h]);
    /* overloaded names get mangled link names (name$N); `name` stays the
     * resolution key. The emission sites use link_name. */
    for(size_t i=0;i<g_prog.nfuncs;i++){
        int dups=0;
        for(size_t j=0;j<g_prog.nfuncs;j++) if(i!=j&&!strcmp(g_prog.funcs[i].name,g_prog.funcs[j].name)){ dups++; break; }
        if(!dups){ g_prog.funcs[i].link_name=NULL; continue; }
        char ln[128]; snprintf(ln,sizeof ln,"%s$%zu",g_prog.funcs[i].name,i);
        g_prog.funcs[i].link_name=strdup(ln);
    }
    return g_prog;
}

/* ---- reflection sidecar JSON (Phase 4): the host-wiring contract ----
 * Emits, per staged/kernel entry: the stage, the resource params (register,
 * kind, Metal slot, element size/stride), the vertex attributes and the
 * output targets. Registered via binc_reflect(); validated against the
 * corpus's D3D-side declarations in tools/hlsl-diff/reflect-check.sh. */
static const char *reflect_stage(int stage){
    return stage==ST_VERTEX?"vertex":stage==ST_FRAGMENT?"fragment":"compute";
}
static const char *reflect_reg(HLSLProg *hp, const char *name, char *buf, size_t n){
    for(size_t i=0;i<hp->ncbufs;i++) if(!strcmp(hp->cbufs[i].name,name)){
        snprintf(buf,n,"b%d",hp->cbufs[i].reg<0?0:hp->cbufs[i].reg); return buf; }
    for(size_t i=0;i<hp->nglobals;i++) if(!strcmp(hp->globals[i].name,name)){
        const char *c=hp->globals[i].ty.kind==T_TEXTURE?"t":hp->globals[i].ty.kind==T_SAMPLER?"s":"u";
        snprintf(buf,n,"%s%d",c,hp->globals[i].reg<0?0:hp->globals[i].reg); return buf; }
    return NULL;
}
static const char *reflect_kind(Type *ty){
    if(ty->is_ptr) return ty->as==AS_CONSTANT?"constant":"buffer";
    if(ty->kind==T_TEXTURE) return ty->tex_cube?"texturecube":ty->tex_array&&ty->tex_dim==2?"texture2d_array":ty->tex_dim==1?"texture1d":ty->tex_dim==3?"texture3d":"texture2d";
    if(ty->kind==T_SAMPLER) return "sampler";
    return "value";
}
static void reflect_struct_fields(FILE *out, Program *prog, const char *tag, const char *prefix){
    for(size_t s=0;s<prog->nstructs;s++) if(!strcmp(prog->structs[s].tag,tag)){
        StructDef *sd=&prog->structs[s];
        for(size_t f=0;f<sd->nfields;f++)
            fprintf(out,"%s    %s{\"name\":\"%s\",\"sem\":\"%s\",\"locn\":%d}\n",prefix,f?",":"",sd->fields[f].name,sd->fields[f].sem?sd->fields[f].sem:"",sd->fields[f].attr_idx);
        return;
    }
}
void binc_reflect(FILE *out, HLSLProg *hp, Program *prog){
    fprintf(out,"{\n  \"entries\": [\n");
    int first_entry=1;
    for(size_t i=0;i<prog->nfuncs;i++){
        Function *fn=&prog->funcs[i];
        if(!fn->is_kernel&&fn->stage==ST_NONE) continue;
        if(!first_entry) fprintf(out,",\n");
        first_entry=0;
        fprintf(out,"    {\"name\":\"%s\",\"stage\":\"%s\",\n",fn->name,reflect_stage(fn->stage));
        /* resources */
        fprintf(out,"      \"resources\": [\n");
        int first_res=1;
        for(size_t p=0;p<fn->nparams;p++){ Param *pr=&fn->params[p];
            if(!(pr->ty.is_ptr||pr->ty.kind==T_TEXTURE||pr->ty.kind==T_SAMPLER)) continue;
            char reg[16]; const char *r=reflect_reg(hp,pr->name,reg,sizeof reg);
            if(!first_res) fprintf(out,",\n");
            first_res=0;
            fprintf(out,"        {\"name\":\"%s\",\"kind\":\"%s\",\"slot\":%zu,\"register\":\"%s\",\"stride\":%d}",
                pr->name,reflect_kind(&pr->ty),p,r?r:"",pr->ty.kind==T_TEXTURE?16:pr->ty.is_ptr?(pr->ty.as==AS_CONSTANT?16:4):4);
        }
        fprintf(out,"\n      ],\n");
        /* stage specifics */
        if(fn->stage==ST_VERTEX){
            fprintf(out,"      \"vertex_inputs\": [\n");
            for(size_t p=0;p<fn->nparams;p++){ Param *pr=&fn->params[p];
                if(pr->ty.kind!=T_STRUCT||pr->ty.is_ptr) continue;
                char tmp[64]; snprintf(tmp,sizeof tmp,""); reflect_struct_fields(out,prog,pr->ty.struct_name,"        ");
            }
            fprintf(out,"      ],\n");
            fprintf(out,"      \"outputs\": [\n");
            if(fn->ret.kind==T_STRUCT){ char tmp[64]; snprintf(tmp,sizeof tmp,""); reflect_struct_fields(out,prog,fn->ret.struct_name,"        "); }
            fprintf(out,"      ]\n");
        } else if(fn->stage==ST_FRAGMENT){
            fprintf(out,"      \"outputs\": [\n");
            if(fn->ret.kind==T_STRUCT){ char tmp[64]; snprintf(tmp,sizeof tmp,""); reflect_struct_fields(out,prog,fn->ret.struct_name,"        "); }
            fprintf(out,"      ]\n");
        } else {
            fprintf(out,"      \"coords\": \"%s\"\n", fn->is_kernel?"dispatch_threads":"");
        }
        fprintf(out,"    }");
    }
    fprintf(out,"\n  ]\n}\n");
}
