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
        if(sem_pref(sem,"TEXCOORD")){ *attr=5; *idx=atoi(sem+8); return; }
        if(sem_pref(sem,"COLOR")){ *attr=5; *idx=8+atoi(sem+5); return; }
        if(sem_eq(sem,"NORMAL")){ *attr=5; *idx=1; return; }
        if(sem_eq(sem,"TANGENT")){ *attr=5; *idx=2; return; }
        if(sem_eq(sem,"BINORMAL")){ *attr=5; *idx=3; return; }
        if(sem_pref(sem,"SV_")){ *attr=5; *idx=0; return; }
        *attr=5; *idx=0; return; /* unknown input semantic: attribute 0 */
    }
    if(sem_eq(sem,"SV_Position")){ *attr=1; return; }
    if(sem_pref(sem,"SV_Target")){ *attr=3; *idx=atoi(sem+9); return; }
    if(sem_eq(sem,"SV_Depth")){ *attr=4; return; }
    if(sem_pref(sem,"TEXCOORD")){ *attr=5; *idx=atoi(sem+8); return; }
    if(sem_pref(sem,"COLOR")){ *attr=5; *idx=atoi(sem+5); return; }
    if(sem_eq(sem,"NORMAL")){ *attr=5; *idx=1; return; }
    if(sem_eq(sem,"TANGENT")){ *attr=5; *idx=2; return; }
    if(sem_eq(sem,"BINORMAL")){ *attr=5; *idx=3; return; }
    if(sem_pref(sem,"SV_")){ *attr=5; *idx=0; return; }
    *attr=5; *idx=0;
}

/* ---- expression/statement rewriting ---- */
typedef struct { const char **names; size_t n; const char *field; const char *base; } Rewrite;

static int rw_matches(const Rewrite *rw, const char *name){
    for(size_t i=0;i<rw->n;i++) if(!strcmp(rw->names[i],name)) return 1;
    return 0;
}
static void rw_expr(Expr *e, const Rewrite *rw){
    if(!e) return;
    if(e->kind==E_IDENT&&rw_matches(rw,e->name)){
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

static void prog_add_struct(StructDef sd){
    g_prog.structs=realloc(g_prog.structs,(g_prog.nstructs+1)*sizeof(StructDef));
    g_prog.structs[g_prog.nstructs++]=sd;
}

/* compute: rewrite the SV_DispatchThreadID / SV_GroupThreadID params into a
 * coord3D param and rewrite body references to .global / .local */
static void lower_compute(Function *fn, HLSLFunc *hf){
    const char *names[8]; size_t nn=0;
    const char *coord_name=NULL; int coord_kind=0; /* 1=global, 2=local */
    for(size_t i=0;i<hf->np;i++){
        HLSLParam *p=&hf->params[i];
        if(!p->sem) continue;
        if(sem_eq(p->sem,"SV_DispatchThreadID")){ names[nn++]=p->name; if(!coord_name){coord_name=p->name;coord_kind=1;} }
        else if(sem_eq(p->sem,"SV_GroupThreadID")){ names[nn++]=p->name; if(!coord_name){coord_name=p->name;coord_kind=2;} }
        else if(sem_eq(p->sem,"SV_GroupID")||sem_eq(p->sem,"SV_GroupIndex"))
            die(hf->line,"SV_GroupID / SV_GroupIndex lower in Phase 5");
    }
    if(!coord_name) return; /* no thread ids: plain kernel */
    Rewrite rw={names,nn,coord_kind==1?"global":"local",NULL};
    rw_block(&fn->body,&rw);
    /* rebuild params from the live list: drop the SV-thread-id params (matched
     * by name), append one coord3D */
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
        if(sd) for(size_t f=0;f<sd->nfields;f++) sem_to_attr(sd->fields[f].sem,&sd->fields[f].attr,&sd->fields[f].attr_idx,0);
    }
}

/* vertex: attribute params -> synthetic stage-in struct; ret struct attrs */
static void lower_vertex(Function *fn, HLSLFunc *hf){
    /* SV_VertexID params use the vertex_id machinery */
    for(size_t i=0;i<hf->np;i++){
        HLSLParam *p=&hf->params[i];
        if(p->sem&&sem_eq(p->sem,"SV_VertexID")){ p->ty.kind=T_UINT32; p->ty.vecn=0; p->ty.as=AS_THREAD; }
    }
    /* gather attribute params (non-pointer, non-coord scalar/vector types) */
    size_t attr_count=0;
    for(size_t i=0;i<hf->np;i++){
        HLSLParam *p=&hf->params[i];
        if(p->ty.is_ptr||p->ty.kind==T_TEXTURE||p->ty.kind==T_SAMPLER) continue;
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
        if(p->sem&&sem_eq(p->sem,"SV_VertexID")) continue;
        Field *f=&sd.fields[fi++];
        f->name=strdup(p->name); f->ty=p->ty;
        f->sem=p->sem?strdup(p->sem):NULL;
        sem_to_attr(p->sem,&f->attr,&f->attr_idx,1);
    }
    sd.nfields=fi;
    prog_add_struct(sd);
    /* replace the attribute params with one stage-in struct param */
    const char **names=malloc(attr_count*sizeof(char*));
    size_t nn=0;
    Param *np2=calloc(hf->np+1,sizeof(Param)); size_t nn2=0;
    for(size_t i=0;i<hf->np;i++){
        HLSLParam *p=&hf->params[i];
        int is_attr = !(p->ty.is_ptr||p->ty.kind==T_TEXTURE||p->ty.kind==T_SAMPLER) &&
                      !(p->sem&&sem_eq(p->sem,"SV_VertexID"));
        if(is_attr){ names[nn++]=p->name; continue; }
        np2[nn2++]=(Param){p->name,p->ty,UN_UNIFORM};
    }
    Param in={0}; in.name=strdup("__in"); in.ty.kind=T_STRUCT; in.ty.struct_name=strdup(tag);
    np2[nn2++]=in;
    free(fn->params); fn->params=np2; fn->nparams=nn2;
    /* rewrite body references: E_IDENT(pname) -> E_FIELD(E_IDENT(__in), pname) */
    for(size_t i=0;i<nn;i++){
        const char *nm=names[i];
        Rewrite rw={&nm,1,nm,"__in"};
        rw_block(&fn->body,&rw);
    }
    free(names);
    /* ret struct: map semantics to attrs */
    if(fn->ret.kind==T_STRUCT){
        for(size_t s=0;s<g_prog.nstructs;s++) if(!strcmp(g_prog.structs[s].tag,fn->ret.struct_name)){
            StructDef *sd=&g_prog.structs[s];
            for(size_t f=0;f<sd->nfields;f++) sem_to_attr(sd->fields[f].sem,&sd->fields[f].attr,&sd->fields[f].attr_idx,0);
        }
    }
}

/* hlsl_build: lower an HLSLProg onto the shared Program AST.
 * entry/profile select the compute/vertex/fragment entry; with stage_all,
 * every function whose return semantics imply a stage gets lowered (render
 * files with multiple stage functions, e.g. shaders.hlsl VS+PS pairs). */
Program hlsl_build(HLSLProg *hp, const char *entry, const char *profile, int stage_all){
    memset(&g_prog,0,sizeof g_prog);
    int is_vs = strncmp(profile,"vs",2)==0;
    int is_ps = strncmp(profile,"ps",2)==0;
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
    /* functions: all of them, plain; the entry gets its stage */
    int entry_found=0;
    for(size_t i=0;i<hp->nfuncs;i++){
        HLSLFunc *hf=&hp->funcs[i];
        Function fn={0};
        fn.name=strdup(hf->name); fn.line=hf->line;
        fn.ret=hf->ret; fn.body=hf->body;
        fn.params=calloc(hf->np?hf->np:1,sizeof(Param));
        fn.nparams=0;
        for(size_t p=0;p<hf->np;p++){
            HLSLParam *hp2=&hf->params[p];
            fn.params[fn.nparams++]=(Param){strdup(hp2->name),hp2->ty,UN_UNIFORM};
        }
        int is_entry = !strcmp(hf->name,entry);
        int s_vs=0, s_ps=0;
        if(stage_all){
            /* infer the stage from the return semantics: SV_Target -> fragment;
             * a struct return carrying SV_Position (or the POSITION semantic)
             * -> vertex (semantics are case-insensitive) */
            if(hf->ret_sem&&sem_pref(hf->ret_sem,"SV_Target")) s_ps=1;
            else if(hf->ret_sem&&(sem_pref(hf->ret_sem,"SV_Position")||sem_pref(hf->ret_sem,"POSITION"))) s_vs=1;
            else if(hf->ret.kind==T_STRUCT){
                StructDef *sd=NULL;
                for(size_t si=0;si<g_prog.nstructs;si++)
                    if(!strcmp(g_prog.structs[si].tag,hf->ret.struct_name)){ sd=&g_prog.structs[si]; break; }
                if(sd) for(size_t fi=0;fi<sd->nfields;fi++){
                    Field *fd=&sd->fields[fi];
                    if(fd->sem&&(sem_pref(fd->sem,"SV_Position")||sem_pref(fd->sem,"POSITION"))) s_vs=1;
                }
            }
        }
        if(is_entry||(stage_all&&(s_vs||s_ps))){
            if(is_entry) entry_found=1;
            if(stage_all){ fn.stage=s_vs?ST_VERTEX:ST_FRAGMENT; }
            else if(is_entry){ fn.stage=is_vs?ST_VERTEX:is_ps?ST_FRAGMENT:ST_NONE; }
            if(fn.stage==ST_VERTEX) lower_vertex(&fn,hf);
            else if(fn.stage==ST_FRAGMENT) lower_fragment(&fn,hf);
            if(is_entry&&!is_vs&&!is_ps&&!stage_all){
                fn.is_kernel=1;
                if(fn.ret.kind!=T_VOID) die(hf->line,"compute entry must return void");
                /* module-level resources (StructuredBuffer<>, RWTexture2D, ...)
                 * become kernel params in declaration order (register parity is
                 * Phase 4; the harness binds by position) */
                for(size_t g=0;g<hp->nglobals;g++){
                    HLSLGlobal *gg=&hp->globals[g];
                    if(!(gg->ty.is_ptr||gg->ty.kind==T_TEXTURE||gg->ty.kind==T_SAMPLER)) continue;
                    fn.params=realloc(fn.params,(fn.nparams+1)*sizeof(Param));
                    fn.params[fn.nparams++]=(Param){strdup(gg->name),gg->ty,UN_UNIFORM};
                }
                lower_compute(&fn,hf);
            }
        }
        g_prog.funcs=realloc(g_prog.funcs,(g_prog.nfuncs+1)*sizeof(Function));
        g_prog.funcs[g_prog.nfuncs++]=fn;
    }
    if(!entry_found&&!stage_all) die(0,"entry point '%s' not found in the shader",entry);
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
