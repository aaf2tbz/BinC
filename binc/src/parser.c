/* parser.c — recursive-descent parser for BinC. */
#include "binc.h"
#include <stdlib.h>
#include <string.h>

static Token *peek(TokStream *ts){ return &ts->toks[ts->i]; }
static Token *advance(TokStream *ts){ return &ts->toks[ts->i++]; }
static int accept(TokStream *ts,TokKind k){ if(peek(ts)->kind==k){ts->i++;return 1;} return 0; }
static void expect(TokStream *ts,TokKind k,const char *w){ if(peek(ts)->kind!=k) die(peek(ts)->line,"expected %s",w); ts->i++; }

static Type parse_type(TokStream *ts){
    Type t={0};
    if(accept(ts,TK_KW_DEVICE))t.as=AS_DEVICE; else if(accept(ts,TK_KW_CONSTANT))t.as=AS_CONSTANT;
    else if(accept(ts,TK_KW_THREADGROUP))t.as=AS_THREADGROUP; else if(accept(ts,TK_KW_THREAD))t.as=AS_THREAD;
    Token *pt=peek(ts);
    if(accept(ts,TK_KW_COORD)){ t.kind=T_COORD; t.coordn=(int)pt->ival; return t; }
    if(accept(ts,TK_KW_GRID_EXTENT)){ t.kind=T_GRID_EXTENT; return t; }
    if(accept(ts,TK_KW_VERTEX_ID)){ t.kind=T_UINT32; t.vecn=0; t.as=AS_THREAD; return t; }
    if(accept(ts,TK_KW_ATOMIC)){
        expect(ts,TK_LT,"<");
        Token *bt=peek(ts); t.kind=T_ATOMIC;
        if(accept(ts,TK_KW_FLOAT))t.atomic_base=T_FLOAT;
        else if(accept(ts,TK_KW_INT))t.atomic_base=T_INT32;
        else if(accept(ts,TK_KW_UINT))t.atomic_base=T_UINT32;
        else die(bt->line,"atomic payload must be float, int, or uint");
        expect(ts,TK_GT,">");
    } else if(accept(ts,TK_KW_FLOAT)){t.kind=T_FLOAT;t.vecn=(int)pt->ival;} else if(accept(ts,TK_KW_HALF))t.kind=T_HALF;
    else if(accept(ts,TK_KW_INT)){t.kind=T_INT32;t.vecn=(int)pt->ival;} else if(accept(ts,TK_KW_UINT)){t.kind=T_UINT32;t.vecn=(int)pt->ival;}
    else if(accept(ts,TK_KW_BOOL))t.kind=T_BOOL; else if(accept(ts,TK_KW_VOID))t.kind=T_VOID;
    else if(peek(ts)->kind==TK_IDENT){ t.kind=T_STRUCT; t.struct_name=strdup(advance(ts)->text); }
    else die(peek(ts)->line,"expected a type");
    if(accept(ts,TK_STAR)){ t.is_ptr=1; if(t.as==0)t.as=AS_DEVICE; }
    return t;
}
static int starts_scalar_type(TokStream *ts){ TokKind k=peek(ts)->kind;
    return k==TK_KW_FLOAT||k==TK_KW_HALF||k==TK_KW_INT||k==TK_KW_UINT||k==TK_KW_BOOL||
           k==TK_KW_COORD||k==TK_KW_GRID_EXTENT; }

static Expr *E(ExprKind k,int line,int col){ Expr *e=calloc(1,sizeof(Expr)); e->kind=k; e->line=line; e->col=col; return e; }
static Expr *parse_expr(TokStream *ts);

/* known struct tags, for disambiguating `Dog d;` local declarations */
static char **stags=NULL; static size_t nstags=0;
static int is_stag(const char *s){ for(size_t i=0;i<nstags;i++) if(!strcmp(stags[i],s)) return 1; return 0; }

static Expr *parse_primary(TokStream *ts){
    Token *t=peek(ts);
    if(t->kind==TK_FCONST){ advance(ts); Expr *e=E(E_FCONST,t->line,t->col); e->fval=t->fval; return e; }
    if(t->kind==TK_ICONST){ advance(ts); Expr *e=E(E_ICONST,t->line,t->col); e->ival=t->ival; return e; }
    if(t->kind==TK_KW_TRUE){ advance(ts); Expr *e=E(E_BOOL,t->line,t->col); e->bval=1; return e; }
    if(t->kind==TK_KW_FALSE){ advance(ts); Expr *e=E(E_BOOL,t->line,t->col); e->bval=0; return e; }
    if((t->kind==TK_KW_FLOAT||t->kind==TK_KW_INT||t->kind==TK_KW_UINT)&&t->ival>1){
        /* vector constructor: float4(...) etc. — synthesize the type name as the callee */
        advance(ts); Expr *e=E(E_IDENT,t->line,t->col); char nm[16];
        snprintf(nm,sizeof nm,"%s%d",t->kind==TK_KW_FLOAT?"float":t->kind==TK_KW_INT?"int":"uint",(int)t->ival);
        e->name=strdup(nm); return e; }
    if(t->kind==TK_IDENT){ advance(ts); Expr *e=E(E_IDENT,t->line,t->col); e->name=strdup(t->text); return e; }
    if(accept(ts,TK_LPAREN)){ Expr *e=parse_expr(ts); expect(ts,TK_RPAREN,")"); return e; }
    die(t->line,"expected an expression");
}
static Expr *parse_postfix(TokStream *ts){
    Expr *e=parse_primary(ts);
    for(;;){
        Token *ot=peek(ts);
        if(ot->kind==TK_LPAREN){ /* calls may also target an atomic method (`acc->add`) */
            if(e->kind!=E_IDENT && !(e->kind==E_FIELD && e->field)) die(ot->line,"callee must be a function or method name");
            advance(ts); Expr *n=E(E_CALL,ot->line,ot->col); n->name=e->kind==E_IDENT?e->name:e->field; if(e->kind!=E_IDENT)n->callee=e;
            Expr **args=NULL; size_t na=0,cap=0;
            while(peek(ts)->kind!=TK_RPAREN){
                if(na==cap){cap=cap?cap*2:4;args=realloc(args,cap*sizeof(Expr*));}
                args[na++]=parse_expr(ts);
                if(!accept(ts,TK_COMMA))break; }
            expect(ts,TK_RPAREN,")");
            n->args=args; n->nargs=na; e=n; }
        else if(ot->kind==TK_LBRACK){ advance(ts); Expr *i=parse_expr(ts); expect(ts,TK_RBRACK,"]");
            Expr *n=E(E_INDEX,ot->line,ot->col); n->operand=e; n->rhs=i; e=n; }
        else if(ot->kind==TK_DOT){ advance(ts); Token *f=peek(ts); expect(ts,TK_IDENT,"field after .");
            Expr *n=E(E_FIELD,ot->line,ot->col); n->operand=e; n->field=strdup(f->text); e=n; }
        else if(ot->kind==TK_ARROW){ advance(ts); Token *f=peek(ts); expect(ts,TK_IDENT,"field after ->");
            Expr *d=E(E_DEREF,ot->line,ot->col); d->operand=e; /* p->f == (*p).f == p[id].f */
            Expr *n=E(E_FIELD,ot->line,ot->col); n->operand=d; n->field=strdup(f->text); e=n; }
        else break;
    }
    return e;
}
/* does the token AFTER '(' begin a scalar/vector numeric type usable as a cast target? */
static int cast_type_start(TokStream *ts){
    TokKind k=(ts->i+1<ts->n)?ts->toks[ts->i+1].kind:TK_EOF;
    return k==TK_KW_FLOAT||k==TK_KW_HALF||k==TK_KW_INT||k==TK_KW_UINT||k==TK_KW_BOOL;
}
static Expr *parse_unary(TokStream *ts){
    Token *ut=peek(ts);
    if(ut->kind==TK_LPAREN&&cast_type_start(ts)){
        advance(ts); Type ty=parse_type(ts);
        if(ty.is_ptr) die(ut->line,"pointer casts are not supported");
        if(ty.kind==T_VOID) die(ut->line,"cannot cast to void");
        expect(ts,TK_RPAREN,")");
        Expr *o=parse_unary(ts); Expr *e=E(E_CAST,ut->line,ut->col); e->cty=ty; e->operand=o; return e;
    }
    if(ut->kind==TK_MINUS){ advance(ts); Expr *o=parse_unary(ts); Expr *e=E(E_NEG,ut->line,ut->col); e->operand=o; return e; }
    if(ut->kind==TK_BANG){ advance(ts); Expr *o=parse_unary(ts); Expr *e=E(E_NOT,ut->line,ut->col); e->operand=o; return e; }
    if(ut->kind==TK_TILDE){ advance(ts); Expr *o=parse_unary(ts); Expr *e=E(E_COMPL,ut->line,ut->col); e->operand=o; return e; }
    if(ut->kind==TK_STAR){ advance(ts); Expr *o=parse_unary(ts); Expr *e=E(E_DEREF,ut->line,ut->col); e->operand=o; return e; }
    return parse_postfix(ts);
}
static Expr *parse_mul(TokStream *ts){
    Expr *l=parse_unary(ts);
    for(;;){ Token *ot=peek(ts); TokKind k=ot->kind; BinOp op;
        if(k==TK_STAR)op=B_MUL; else if(k==TK_SLASH)op=B_DIV; else if(k==TK_PERCENT)op=B_MOD;
        else if(k==TK_SHL)op=B_SHL; else if(k==TK_SHR)op=B_SHR; else break;
        advance(ts); Expr *r=parse_unary(ts); Expr *e=E(E_BIN,ot->line,ot->col); e->bop=op; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_add(TokStream *ts){
    Expr *l=parse_mul(ts);
    for(;;){ Token *ot=peek(ts); TokKind k=ot->kind; if(k!=TK_PLUS&&k!=TK_MINUS)break; BinOp op=k==TK_PLUS?B_ADD:B_SUB;
        advance(ts); Expr *r=parse_mul(ts); Expr *e=E(E_BIN,ot->line,ot->col); e->bop=op; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_rel(TokStream *ts){
    Expr *l=parse_add(ts);
    for(;;){ Token *ot=peek(ts); TokKind k=ot->kind; CmpOp op;
        if(k==TK_LT)op=C_LT; else if(k==TK_LE)op=C_LE; else if(k==TK_GT)op=C_GT; else if(k==TK_GE)op=C_GE; else break;
        advance(ts); Expr *r=parse_add(ts); Expr *e=E(E_CMP,ot->line,ot->col); e->cmp=op; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_eq(TokStream *ts){
    Expr *l=parse_rel(ts);
    for(;;){ Token *ot=peek(ts); TokKind k=ot->kind; if(k!=TK_EQEQ&&k!=TK_NEQ)break; CmpOp op=k==TK_EQEQ?C_EQ:C_NE;
        advance(ts); Expr *r=parse_rel(ts); Expr *e=E(E_CMP,ot->line,ot->col); e->cmp=op; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_bitor(TokStream *ts);
static Expr *parse_bitxor(TokStream *ts);
static Expr *parse_bitand(TokStream *ts);
static Expr *parse_bitor(TokStream *ts){
    Expr *l=parse_bitxor(ts);
    while(peek(ts)->kind==TK_PIPE){ Token *ot=peek(ts); advance(ts); Expr *r=parse_bitxor(ts); Expr *e=E(E_BIN,ot->line,ot->col); e->bop=B_OR; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_bitxor(TokStream *ts){
    Expr *l=parse_bitand(ts);
    while(peek(ts)->kind==TK_CARET){ Token *ot=peek(ts); advance(ts); Expr *r=parse_bitand(ts); Expr *e=E(E_BIN,ot->line,ot->col); e->bop=B_XOR; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_bitand(TokStream *ts){
    Expr *l=parse_eq(ts);
    while(peek(ts)->kind==TK_AMP){ Token *ot=peek(ts); advance(ts); Expr *r=parse_eq(ts); Expr *e=E(E_BIN,ot->line,ot->col); e->bop=B_AND; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_and(TokStream *ts){
    Expr *l=parse_bitor(ts);
    while(peek(ts)->kind==TK_AND){ Token *ot=peek(ts); advance(ts); Expr *r=parse_bitor(ts); Expr *e=E(E_LOG,ot->line,ot->col); e->log=L_AND; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_or(TokStream *ts){
    Expr *l=parse_and(ts);
    while(peek(ts)->kind==TK_OR){ Token *ot=peek(ts); advance(ts); Expr *r=parse_and(ts); Expr *e=E(E_LOG,ot->line,ot->col); e->log=L_OR; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_ternary(TokStream *ts){
    Expr *c=parse_or(ts);
    Token *qt=peek(ts);
    if(qt->kind!=TK_QUESTION) return c;
    advance(ts);
    Expr *a=parse_expr(ts);           /* full expression: allows nested ternaries */
    expect(ts,TK_COLON,":");
    Expr *b=parse_ternary(ts);        /* right-associative */
    Expr *e=E(E_TERNARY,qt->line,qt->col); e->operand=c; e->lhs=a; e->rhs=b; return e;
}
static Expr *parse_assign(TokStream *ts){
    Expr *l=parse_ternary(ts); Token *ot=peek(ts); TokKind k=ot->kind; AssignOp op;
    if(k==TK_EQ)op=A_ASSIGN; else if(k==TK_PLUSEQ)op=A_ADDEQ; else if(k==TK_MINUSEQ)op=A_SUBEQ;
    else if(k==TK_STAREQ)op=A_MULEQ; else if(k==TK_SLASHEQ)op=A_DIVEQ; else if(k==TK_MODEQ)op=A_MODEQ;
    else if(k==TK_AMPEQ)op=A_ANDEQ; else if(k==TK_PIPEEQ)op=A_OREQ; else if(k==TK_CARETEQ)op=A_XOREQ;
    else if(k==TK_SHLEQ)op=A_SHLEQ; else if(k==TK_SHREQ)op=A_SHREQ;
    else return l;
    advance(ts); Expr *r=parse_assign(ts); Expr *e=E(E_ASSIGN,ot->line,ot->col); e->aop=op; e->operand=l; e->rhs=r; return e;
}
static Expr *parse_expr(TokStream *ts){ return parse_assign(ts); }

static Stmt parse_stmt(TokStream *ts);
static Block parse_braced(TokStream *ts){ /* assumes '{' consumed */
    Stmt *s=NULL; size_t n=0,cap=0;
    while(peek(ts)->kind!=TK_RBRACE&&peek(ts)->kind!=TK_EOF){
        if(n==cap){cap=cap?cap*2:8;s=realloc(s,cap*sizeof(Stmt));} s[n++]=parse_stmt(ts); }
    expect(ts,TK_RBRACE,"}");
    return (Block){s,n};
}
static Block parse_block_or_stmt(TokStream *ts){
    if(accept(ts,TK_LBRACE)) return parse_braced(ts);
    Stmt *s=malloc(sizeof(Stmt)); s[0]=parse_stmt(ts); return (Block){s,1};
}
static Stmt parse_stmt(TokStream *ts){
    Token *kt=peek(ts);
    if(kt->kind==TK_KW_RETURN){ advance(ts); Stmt st={0}; st.kind=S_RETURN; st.line=kt->line; st.col=kt->col;
        if(peek(ts)->kind!=TK_SEMI) st.expr=parse_expr(ts);
        expect(ts,TK_SEMI,";"); return st; }
    if(kt->kind==TK_KW_BREAK){ advance(ts); expect(ts,TK_SEMI,";"); Stmt st={0}; st.kind=S_BREAK; st.line=kt->line; st.col=kt->col; return st; }
    if(kt->kind==TK_KW_CONTINUE){ advance(ts); expect(ts,TK_SEMI,";"); Stmt st={0}; st.kind=S_CONTINUE; st.line=kt->line; st.col=kt->col; return st; }
    if(kt->kind==TK_KW_IF){
        advance(ts); expect(ts,TK_LPAREN,"("); Expr *cond=parse_expr(ts); expect(ts,TK_RPAREN,")");
        Stmt st={0}; st.kind=S_IF; st.line=kt->line; st.col=kt->col; st.cond=cond; st.then_b=parse_block_or_stmt(ts);
        st.else_b = accept(ts,TK_KW_ELSE)? parse_block_or_stmt(ts) : ((Block){NULL,0});
        return st;
    }
    if(kt->kind==TK_KW_WHILE){
        advance(ts); expect(ts,TK_LPAREN,"("); Expr *cond=parse_expr(ts); expect(ts,TK_RPAREN,")");
        Stmt st={0}; st.kind=S_WHILE; st.line=kt->line; st.col=kt->col; st.cond=cond; st.then_b=parse_block_or_stmt(ts); return st;
    }
    if(kt->kind==TK_KW_DO){
        advance(ts); Stmt st={0}; st.kind=S_DOWHILE; st.line=kt->line; st.col=kt->col;
        st.then_b=parse_block_or_stmt(ts);
        expect(ts,TK_KW_WHILE,"while"); expect(ts,TK_LPAREN,"(");
        st.cond=parse_expr(ts); expect(ts,TK_RPAREN,")"); expect(ts,TK_SEMI,";");
        return st;
    }
    if(kt->kind==TK_KW_FOR){
        advance(ts); expect(ts,TK_LPAREN,"("); Stmt st={0}; st.kind=S_FOR; st.line=kt->line; st.col=kt->col;
        if(peek(ts)->kind!=TK_SEMI){
            Stmt *fi=malloc(sizeof(Stmt)); memset(fi,0,sizeof *fi);
            if(starts_scalar_type(ts)){ Type ty=parse_type(ts); Token *nm=peek(ts); expect(ts,TK_IDENT,"name");
                Expr *init=NULL; if(accept(ts,TK_EQ))init=parse_expr(ts);
                fi->kind=S_DECL; fi->line=nm->line; fi->col=nm->col; fi->ty=ty; fi->name=strdup(nm->text); fi->init=init; }
            else { fi->kind=S_EXPR; fi->line=peek(ts)->line; fi->expr=parse_expr(ts); }
            st.for_init=fi;
        }
        expect(ts,TK_SEMI,";");
        st.for_cond = peek(ts)->kind!=TK_SEMI? parse_expr(ts):NULL;
        expect(ts,TK_SEMI,";");
        st.for_incr = peek(ts)->kind!=TK_RPAREN? parse_expr(ts):NULL;
        expect(ts,TK_RPAREN,")");
        st.then_b=parse_block_or_stmt(ts); return st;
    }
    if(kt->kind==TK_LBRACE){ advance(ts); Block b=parse_braced(ts); Stmt st={0}; st.kind=S_BLOCK; st.line=kt->line; st.col=kt->col; st.then_b=b; return st; }
    if(kt->kind==TK_IDENT&&is_stag(kt->text)&&ts->toks[ts->i+1].kind==TK_IDENT){
        /* struct local: `Dog d;` */
        Type ty={0}; ty.kind=T_STRUCT; ty.struct_name=strdup(advance(ts)->text);
        Token *nm=peek(ts); expect(ts,TK_IDENT,"name"); expect(ts,TK_SEMI,";");
        Stmt st={0}; st.kind=S_DECL; st.line=nm->line; st.col=nm->col; st.ty=ty; st.name=strdup(nm->text); return st;
    }
    if(starts_scalar_type(ts)){
        Type ty=parse_type(ts); Token *nm=peek(ts); expect(ts,TK_IDENT,"name");
        Expr *init=NULL; if(accept(ts,TK_EQ))init=parse_expr(ts); expect(ts,TK_SEMI,";");
        Stmt st={0}; st.kind=S_DECL; st.line=nm->line; st.col=nm->col; st.ty=ty; st.name=strdup(nm->text); st.init=init; return st;
    }
    Expr *e=parse_expr(ts); expect(ts,TK_SEMI,";"); Stmt st={0}; st.kind=S_EXPR; st.line=e->line; st.col=e->col; st.expr=e; return st;
}

static void parse_function(TokStream *ts, Program *prog){
    Stage stage=ST_NONE; if(accept(ts,TK_KW_VERTEX))stage=ST_VERTEX; else if(accept(ts,TK_KW_FRAGMENT))stage=ST_FRAGMENT;
    int is_kernel=accept(ts,TK_KW_KERNEL);
    Type ret=parse_type(ts);
    if(stage!=ST_NONE&&is_kernel) die(peek(ts)->line,"render stages cannot also be kernels");
    if(is_kernel&&ret.kind!=T_VOID) die(peek(ts)->line,"kernel functions must return void");
    if(ret.kind==T_STRUCT) die(peek(ts)->line,"struct-by-value return not supported");
    Token *nm=peek(ts); expect(ts,TK_IDENT,"function name"); expect(ts,TK_LPAREN,"(");
    Param *params=NULL; size_t np=0,cap=0;
    while(peek(ts)->kind!=TK_RPAREN){
        Uniformity un=UN_UNIFORM; if(accept(ts,TK_KW_VARYING))un=UN_VARYING; else accept(ts,TK_KW_UNIFORM);
        Type ty=parse_type(ts); Token *pn=peek(ts); expect(ts,TK_IDENT,"param name");
        if(accept(ts,TK_LBRACK)){
            Token *dn=peek(ts); expect(ts,TK_ICONST,"array extent"); ty.array_n=(int)dn->ival; expect(ts,TK_RBRACK,"]");
            if(accept(ts,TK_LBRACK)){ Token *dm=peek(ts); expect(ts,TK_ICONST,"array extent"); ty.array_m=(int)dm->ival; expect(ts,TK_RBRACK,"]"); }
            if(ty.as!=AS_THREADGROUP) die(pn->line,"only threadgroup parameters may be arrays");
        }
        if(ty.kind==T_STRUCT&&!ty.is_ptr) die(pn->line,"struct-by-value parameter not supported");
        if(ty.kind==T_COORD&&ty.is_ptr) die(pn->line,"coordinates cannot be pointers");
        if(np==cap){cap=cap?cap*2:4;params=realloc(params,cap*sizeof(Param));}
        params[np++]=(Param){strdup(pn->text),ty,un};
        if(!accept(ts,TK_COMMA))break;
    }
    expect(ts,TK_RPAREN,")"); expect(ts,TK_LBRACE,"{"); Block body=parse_braced(ts);
    /* An explicit coordinate is itself a launch domain, so `void f(..., coord1D i)`
     * is kernel syntax even without the optional `kernel` spelling. */
    int coords=0; for(size_t i=0;i<np;i++) if(params[i].ty.kind==T_COORD) coords++;
    if(coords>1) die(peek(ts)->line,"a kernel may have only one coordinate domain parameter");
    if(coords) is_kernel=1;
    prog->funcs=realloc(prog->funcs,(prog->nfuncs+1)*sizeof(Function));
    prog->funcs[prog->nfuncs++]=(Function){strdup(nm->text),params,np,body,is_kernel,stage,ret,nm->line};
}
static void parse_struct(TokStream *ts, Program *prog){
    Token *tag=peek(ts); expect(ts,TK_IDENT,"struct tag"); expect(ts,TK_LBRACE,"{");
    Field *f=NULL; size_t n=0,cap=0;
    while(peek(ts)->kind!=TK_RBRACE){
        Type ty=parse_type(ts); if(ty.is_ptr) die(peek(ts)->line,"pointer fields unsupported");
        do{ Token *fn=peek(ts); expect(ts,TK_IDENT,"field name");
            if(n==cap){cap=cap?cap*2:8;f=realloc(f,cap*sizeof(Field));} f[n++]=(Field){strdup(fn->text),ty};
        } while(accept(ts,TK_COMMA)); expect(ts,TK_SEMI,";");
    }
    expect(ts,TK_RBRACE,"}"); expect(ts,TK_SEMI,";");
    prog->structs=realloc(prog->structs,(prog->nstructs+1)*sizeof(StructDef));
    prog->structs[prog->nstructs++]=(StructDef){strdup(tag->text),f,n};
    stags=realloc(stags,(nstags+1)*sizeof(char*)); stags[nstags++]=prog->structs[prog->nstructs-1].tag;
}
/* tokens that may legally begin a statement or top-level construct; used by error recovery */
static int stmt_start(Token *t){
    switch(t->kind){
    case TK_KW_STRUCT: case TK_KW_RETURN: case TK_KW_BREAK: case TK_KW_CONTINUE:
    case TK_KW_IF: case TK_KW_WHILE: case TK_KW_FOR: case TK_KW_DO:
    case TK_KW_VERTEX: case TK_KW_FRAGMENT: case TK_KW_KERNEL:
    case TK_LBRACE: case TK_SEMI: case TK_KW_DEVICE: case TK_KW_CONSTANT:
    case TK_KW_THREADGROUP: case TK_KW_THREAD: case TK_KW_UNIFORM: case TK_KW_VARYING:
    case TK_KW_FLOAT: case TK_KW_HALF: case TK_KW_INT: case TK_KW_UINT: case TK_KW_BOOL:
    case TK_KW_VOID: case TK_KW_COORD: case TK_KW_GRID_EXTENT: case TK_KW_ATOMIC:
        return 1;
    default:
        return t->kind==TK_IDENT && is_stag(t->text);
    }
}
/* skip forward past a failed construct so parsing can continue and report more errors.
 * always consumes at least one token, so recovery is guaranteed to make progress. */
static void recover_skip(TokStream *ts){
    int depth=0, consumed=0;
    for(;;){
        Token *t=peek(ts);
        if(t->kind==TK_EOF) return;
        if(consumed && depth==0 && stmt_start(t)) return;
        if(t->kind==TK_LBRACE) depth++;
        else if(t->kind==TK_RBRACE){ if(depth==0){ advance(ts); return; } depth--; }
        advance(ts); consumed=1;
    }
}
Program parse_program(TokStream *ts){
    /* static so the struct and stream survive the longjmp in die() with defined values */
    static Program prog;
    static TokStream *cur;
    prog.structs=NULL; prog.nstructs=0; prog.funcs=NULL; prog.nfuncs=0;
    cur=ts;
    jmp_buf env;
    g_recover=&env;
    while(peek(cur)->kind!=TK_EOF){
        if(setjmp(env)==0){
            if(peek(cur)->kind==TK_KW_STRUCT){ advance(cur); parse_struct(cur,&prog); } else parse_function(cur,&prog);
        } else {
            recover_skip(cur);
            /* swallow stray closers/semicolons left by the failed construct */
            while(peek(cur)->kind==TK_SEMI || peek(cur)->kind==TK_RBRACE) advance(cur);
        }
    }
    g_recover=NULL;
    return prog;
}
