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
static void iw_expr(Expr *e, const char *fnname);
static void iw_block(Block *b, const char *fnname);
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
static void iw_stmt(Stmt *st, const char *fnname){
    switch(st->kind){
    case S_EXPR: iw_expr(st->expr,fnname); break;
    case S_DECL: iw_expr(st->init,fnname); break;
    case S_RETURN: iw_expr(st->expr,fnname); break;
    case S_IF: iw_expr(st->cond,fnname); iw_block(&st->then_b,fnname); iw_block(&st->else_b,fnname); break;
    case S_WHILE: case S_DOWHILE: iw_expr(st->cond,fnname); iw_block(&st->then_b,fnname); break;
    case S_FOR:
        if(st->for_init) iw_stmt(st->for_init,fnname);
        iw_expr(st->for_cond,fnname); iw_expr(st->for_incr,fnname); iw_block(&st->then_b,fnname); break;
    case S_SWITCH:
        iw_expr(st->sw_cond,fnname);
        for(size_t i=0;i<st->ncases;i++){ iw_expr(st->cases[i].val,fnname); iw_block(&st->cases[i].body,fnname); }
        iw_block(&st->def_body,fnname); break;
    case S_BLOCK: iw_block(&st->then_b,fnname); break;
    default: break;
    }
}
static void iw_block(Block *b, const char *fnname){
    for(size_t i=0;i<b->n;i++) iw_stmt(&b->stmts[i],fnname);
}
static void iw_expr(Expr *e, const char *fnname){
    if(!e) return;
    if(e->kind==E_CALL&&e->name&&!strcmp(e->name,fnname)){
        if(e->nargs<1) die(0,"inout call needs at least one argument");
        /* a = f(a, b, ...) — the receiver is arg0, the call keeps every arg */
        Expr *receiver=rw_copy(e->args[0]);
        Expr *as=E(E_ASSIGN,e->line,e->col); as->aop=A_ASSIGN; as->operand=receiver; as->rhs=rw_copy(e);
        if(getenv("BINC_DEBUG_IW")) fprintf(stderr,"DBG iw: call->assign operand=%s kind=%d\n",receiver->name,receiver->kind);
        *e=*as;
        return;
    }
    iw_expr(e->operand,fnname); iw_expr(e->lhs,fnname); iw_expr(e->rhs,fnname);
    if(e->callee) iw_expr(e->callee,fnname);
    for(size_t i=0;i<e->nargs;i++) iw_expr(e->args[i],fnname);
}
static void prog_add_struct(StructDef sd){
    g_prog.structs=realloc(g_prog.structs,(g_prog.nstructs+1)*sizeof(StructDef));
    g_prog.structs[g_prog.nstructs++]=sd;
}

/* compute: rewrite the SV_DispatchThreadID / SV_GroupThreadID params into a
 * coord3D param and rewrite body references to .global / .local */
static void lower_compute(Function *fn, HLSLFunc *hf){
    const char *names[8]; size_t nn=0;
    const char *coord_name=NULL; int coord_kind __attribute__((unused)) =0; /* 1=global, 2=local */
    for(size_t i=0;i<hf->np;i++){
        HLSLParam *p=&hf->params[i];
        if(!p->sem) continue;
        if(sem_eq(p->sem,"SV_DispatchThreadID")){ names[nn++]=p->name; if(!coord_name){coord_name=p->name;coord_kind=1;} }
        else if(sem_eq(p->sem,"SV_GroupThreadID")){ names[nn++]=p->name; if(!coord_name){coord_name=p->name;coord_kind=2;} }
        else if(sem_eq(p->sem,"SV_GroupID")){ names[nn++]=p->name; if(!coord_name){coord_name=p->name;coord_kind=3;} }
        else if(sem_eq(p->sem,"SV_GroupIndex")){ names[nn++]=p->name; if(!coord_name){coord_name=p->name;coord_kind=4;} }
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
static char **iw_helpers=NULL; static size_t niw=0;
Program hlsl_build(HLSLProg *hp, const char *entry, const char *profile, int stage_all){
    memset(&g_prog,0,sizeof g_prog);
    fprintf(stderr,"DBG build: entry=%s nglobals=%zu\n",entry,hp->nglobals);
    int is_vs = strncmp(profile,"vs",2)==0;
    int is_ps = strncmp(profile,"ps",2)==0;
    int is_gs = strncmp(profile,"gs",2)==0;
    /* module constants: static const globals -> ConstDef */
    for(size_t i=0;i<hp->nglobals;i++){
        HLSLGlobal *gg=&hp->globals[i];
        if(gg->is_const&&gg->has_init&&!gg->is_groupshared){
            ConstDef cd={0};
            cd.name=strdup(gg->name); cd.ty=gg->ty; cd.line=gg->line;
            cd.is_int=gg->is_int; cd.ival=gg->ival; cd.fval=gg->fval;
            g_prog.consts=realloc(g_prog.consts,(g_prog.nconsts+1)*sizeof(ConstDef));
            g_prog.consts[g_prog.nconsts++]=cd;
        }
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
            if(ft.vecn==2||ft.vecn==3){ ft.array_n=ft.vecn; ft.vecn=0; } /* packed vector */
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
    Rewrite unif_rw[64]; size_t nu=0; /* D3D9 uniform rewrites (built below) */
    /* D3D9 uniforms: non-const scalar/vector globals pack into one const
     * struct (like a cbuffer); the harness binds it at the register index
     * that follows the declared resources. */
    {
        size_t nuf=0; Field uf[64]; int ureg=1000;
        for(size_t i=0;i<hp->nglobals;i++){
            HLSLGlobal *gg=&hp->globals[i];
            if(getenv("BINC_DEBUG_D3D9")) fprintf(stderr,"DBG unif: %s kind=%d isc=%d vecn=%d an=%d\n",gg->name,gg->ty.kind,gg->is_const,gg->ty.vecn,gg->ty.array_n);
            if(gg->is_const||gg->is_groupshared) continue; /* groupshared stays threadgroup memory */
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
    /* functions: all of them, plain; the entry gets its stage */
    int entry_found=0;
    for(size_t i=0;i<hp->nfuncs;i++){
        HLSLFunc *hf=&hp->funcs[i];
        Function fn={0};
        fn.name=strdup(hf->name); fn.line=hf->line;
        fn.ret=hf->ret; fn.body=hf->body;
        rn_block(&fn.body); /* HLSL barriers -> sync */
        ax_block(&fn.body); /* Interlocked* -> the atomic method form */
        fn.params=calloc(hf->np?hf->np:1,sizeof(Param));
        fn.nparams=0;
        for(size_t p=0;p<hf->np;p++){
            HLSLParam *hp2=&hf->params[p];
            fn.params[fn.nparams++]=(Param){strdup(hp2->name),hp2->ty,UN_UNIFORM};
        }
        /* inout/out helpers (Phase 5): single inout on a void function becomes a
         * value return; the call sites are rewritten to `a = f(b, ...)` */
        {
            int io=-1;
            for(size_t p=0;p<hf->np;p++) if(hf->params[p].inq==2||hf->params[p].inq==3){
                if(io>=0) die(hf->line,"multiple inout params not supported yet"); io=(int)p;
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
                /* remember the helper so every function's call sites get rewritten */
                iw_helpers=realloc(iw_helpers,(niw+1)*sizeof(char*));
                iw_helpers[niw++]=strdup(hf->name);
            }
        }
        int is_entry = !strcmp(hf->name,entry);
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
            /* resources (cbuffers + typed globals) become params in register
             * order; cbuffer field references rewrite to struct-field access */
            for(size_t r=0;r<nres;r++){
                fn.params=realloc(fn.params,(fn.nparams+1)*sizeof(Param));
                fn.params[fn.nparams++]=(Param){strdup(res[r].name),res[r].ty,UN_UNIFORM};
            }
            /* uniform params (D3D9 per-draw constants) fold into __uniforms:
             * drop them from the stage signature; the body references are
             * rewritten to __uniforms.<name> by the unif_rw pass below. */
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
                    Rewrite rw={&fname,1,fname,cb->name,NULL};
                    rw_block(&fn.body,&rw);
                }
            }
            for(size_t ui=0;ui<nu;ui++) rw_block(&fn.body,&unif_rw[ui]);
            if(fn.stage==ST_VERTEX) lower_vertex(&fn,hf);
            else if(fn.stage==ST_FRAGMENT) lower_fragment(&fn,hf);
            else if(fn.stage==ST_GEOMETRY){
                /* GS lowering to the Metal mesh stage is the in-scope next chunk
                 * (probed: this toolchain dropped air.amplification; the mesh
                 * path exists via metal_mesh). Parse/classification land here. */
                die(hf->line,"geometry shader lowering (Metal mesh) not yet wired — GS parses, stage classified");
            }
            /* D3D9 tex2D family: rewrite into the texture-method form */
            tex2d_block(&fn.body,hp);
            if(is_entry&&!is_vs&&!is_ps&&!is_gs&&!stage_all){
                fn.is_kernel=1;
                if(fn.ret.kind!=T_VOID) die(hf->line,"compute entry must return void");
                lower_compute(&fn,hf);
            }
        }
        g_prog.funcs=realloc(g_prog.funcs,(g_prog.nfuncs+1)*sizeof(Function));
        g_prog.funcs[g_prog.nfuncs++]=fn;
    }
    if(!entry_found&&!stage_all) die(0,"entry point '%s' not found in the shader",entry);
    /* rewrite every inout-helper call site across all functions */
    for(size_t h=0;h<niw;h++) for(size_t i=0;i<g_prog.nfuncs;i++) iw_block(&g_prog.funcs[i].body,iw_helpers[h]);
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
    if(ty->kind==T_TEXTURE) return "texture2d";
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
