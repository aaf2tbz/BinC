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
    if(accept(ts,TK_KW_FLOAT))t.kind=T_FLOAT; else if(accept(ts,TK_KW_HALF))t.kind=T_HALF;
    else if(accept(ts,TK_KW_INT))t.kind=T_INT32; else if(accept(ts,TK_KW_UINT))t.kind=T_UINT32;
    else if(accept(ts,TK_KW_BOOL))t.kind=T_BOOL; else if(accept(ts,TK_KW_VOID))t.kind=T_VOID;
    else if(peek(ts)->kind==TK_IDENT){ t.kind=T_STRUCT; t.struct_name=strdup(advance(ts)->text); }
    else die(peek(ts)->line,"expected a type");
    if(accept(ts,TK_STAR)){ t.is_ptr=1; if(t.as==0)t.as=AS_DEVICE; }
    return t;
}
static int starts_scalar_type(TokStream *ts){ TokKind k=peek(ts)->kind;
    return k==TK_KW_FLOAT||k==TK_KW_HALF||k==TK_KW_INT||k==TK_KW_UINT||k==TK_KW_BOOL; }

static Expr *E(ExprKind k){ Expr *e=calloc(1,sizeof(Expr)); e->kind=k; return e; }
static Expr *parse_expr(TokStream *ts);

static Expr *parse_primary(TokStream *ts){
    Token *t=peek(ts);
    if(t->kind==TK_FCONST){ advance(ts); Expr *e=E(E_FCONST); e->fval=t->fval; return e; }
    if(t->kind==TK_ICONST){ advance(ts); Expr *e=E(E_ICONST); e->ival=t->ival; return e; }
    if(t->kind==TK_KW_TRUE){ advance(ts); Expr *e=E(E_BOOL); e->bval=1; return e; }
    if(t->kind==TK_KW_FALSE){ advance(ts); Expr *e=E(E_BOOL); e->bval=0; return e; }
    if(t->kind==TK_IDENT){ advance(ts); Expr *e=E(E_IDENT); e->name=strdup(t->text); return e; }
    if(accept(ts,TK_LPAREN)){ Expr *e=parse_expr(ts); expect(ts,TK_RPAREN,")"); return e; }
    die(t->line,"expected an expression");
}
static Expr *parse_postfix(TokStream *ts){
    Expr *e=parse_primary(ts);
    while(peek(ts)->kind==TK_ARROW){ advance(ts); Token *f=peek(ts); expect(ts,TK_IDENT,"field after ->");
        Expr *n=E(E_FIELD); n->operand=e; n->field=strdup(f->text); e=n; }
    return e;
}
static Expr *parse_unary(TokStream *ts){
    if(accept(ts,TK_MINUS)){ Expr *o=parse_unary(ts); Expr *e=E(E_NEG); e->operand=o; return e; }
    if(accept(ts,TK_BANG)){ Expr *o=parse_unary(ts); Expr *e=E(E_NOT); e->operand=o; return e; }
    if(accept(ts,TK_STAR)){ Expr *o=parse_unary(ts); Expr *e=E(E_DEREF); e->operand=o; return e; }
    return parse_postfix(ts);
}
static Expr *parse_mul(TokStream *ts){
    Expr *l=parse_unary(ts);
    for(;;){ TokKind k=peek(ts)->kind; BinOp op;
        if(k==TK_STAR)op=B_MUL; else if(k==TK_SLASH)op=B_DIV; else if(k==TK_PERCENT)op=B_MOD; else break;
        advance(ts); Expr *r=parse_unary(ts); Expr *e=E(E_BIN); e->bop=op; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_add(TokStream *ts){
    Expr *l=parse_mul(ts);
    for(;;){ TokKind k=peek(ts)->kind; if(k!=TK_PLUS&&k!=TK_MINUS)break; BinOp op=k==TK_PLUS?B_ADD:B_SUB;
        advance(ts); Expr *r=parse_mul(ts); Expr *e=E(E_BIN); e->bop=op; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_rel(TokStream *ts){
    Expr *l=parse_add(ts);
    for(;;){ TokKind k=peek(ts)->kind; CmpOp op;
        if(k==TK_LT)op=C_LT; else if(k==TK_LE)op=C_LE; else if(k==TK_GT)op=C_GT; else if(k==TK_GE)op=C_GE; else break;
        advance(ts); Expr *r=parse_add(ts); Expr *e=E(E_CMP); e->cmp=op; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_eq(TokStream *ts){
    Expr *l=parse_rel(ts);
    for(;;){ TokKind k=peek(ts)->kind; if(k!=TK_EQEQ&&k!=TK_NEQ)break; CmpOp op=k==TK_EQEQ?C_EQ:C_NE;
        advance(ts); Expr *r=parse_rel(ts); Expr *e=E(E_CMP); e->cmp=op; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_and(TokStream *ts){
    Expr *l=parse_eq(ts);
    while(peek(ts)->kind==TK_AND){ advance(ts); Expr *r=parse_eq(ts); Expr *e=E(E_LOG); e->log=L_AND; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_or(TokStream *ts){
    Expr *l=parse_and(ts);
    while(peek(ts)->kind==TK_OR){ advance(ts); Expr *r=parse_and(ts); Expr *e=E(E_LOG); e->log=L_OR; e->lhs=l; e->rhs=r; l=e; }
    return l;
}
static Expr *parse_assign(TokStream *ts){
    Expr *l=parse_or(ts); TokKind k=peek(ts)->kind; AssignOp op;
    if(k==TK_EQ)op=A_ASSIGN; else if(k==TK_PLUSEQ)op=A_ADDEQ; else if(k==TK_MINUSEQ)op=A_SUBEQ;
    else if(k==TK_STAREQ)op=A_MULEQ; else if(k==TK_SLASHEQ)op=A_DIVEQ; else if(k==TK_MODEQ)op=A_MODEQ;
    else return l;
    advance(ts); Expr *r=parse_assign(ts); Expr *e=E(E_ASSIGN); e->aop=op; e->operand=l; e->rhs=r; return e;
}
static Expr *parse_expr(TokStream *ts){ return parse_assign(ts); }

static Stmt parse_stmt(TokStream *ts);
static Block parse_braced(TokStream *ts){ /* assumes '{' consumed */
    Stmt *s=NULL; size_t n=0,cap=0;
    while(peek(ts)->kind!=TK_RBRACE&&peek(ts)->kind!=TK_EOF){
        if(n==cap){cap=cap?cap*2:8;s=realloc(s,cap*sizeof(Stmt));} s[n++]=parse_stmt(ts); }
    expect(ts,TK_RBRACE,"}"); return (Block){s,n};
}
static Block parse_block_or_stmt(TokStream *ts){
    if(accept(ts,TK_LBRACE)) return parse_braced(ts);
    Stmt *s=malloc(sizeof(Stmt)); s[0]=parse_stmt(ts); return (Block){s,1};
}
static Stmt parse_stmt(TokStream *ts){
    if(accept(ts,TK_KW_RETURN)){ expect(ts,TK_SEMI,";"); Stmt st={0}; st.kind=S_RETURN; return st; }
    if(peek(ts)->kind==TK_KW_IF){
        advance(ts); expect(ts,TK_LPAREN,"("); Expr *cond=parse_expr(ts); expect(ts,TK_RPAREN,")");
        Stmt st={0}; st.kind=S_IF; st.cond=cond; st.then_b=parse_block_or_stmt(ts);
        st.else_b = accept(ts,TK_KW_ELSE)? parse_block_or_stmt(ts) : ((Block){NULL,0});
        return st;
    }
    if(peek(ts)->kind==TK_KW_WHILE){
        advance(ts); expect(ts,TK_LPAREN,"("); Expr *cond=parse_expr(ts); expect(ts,TK_RPAREN,")");
        Stmt st={0}; st.kind=S_WHILE; st.cond=cond; st.then_b=parse_block_or_stmt(ts); return st;
    }
    if(peek(ts)->kind==TK_KW_FOR){
        advance(ts); expect(ts,TK_LPAREN,"("); Stmt st={0}; st.kind=S_FOR;
        if(peek(ts)->kind!=TK_SEMI){
            Stmt *fi=malloc(sizeof(Stmt)); memset(fi,0,sizeof *fi);
            if(starts_scalar_type(ts)){ Type ty=parse_type(ts); Token *nm=peek(ts); expect(ts,TK_IDENT,"name");
                Expr *init=NULL; if(accept(ts,TK_EQ))init=parse_expr(ts);
                fi->kind=S_DECL; fi->ty=ty; fi->name=strdup(nm->text); fi->init=init; }
            else { fi->kind=S_EXPR; fi->expr=parse_expr(ts); }
            st.for_init=fi;
        }
        expect(ts,TK_SEMI,";");
        st.for_cond = peek(ts)->kind!=TK_SEMI? parse_expr(ts):NULL;
        expect(ts,TK_SEMI,";");
        st.for_incr = peek(ts)->kind!=TK_RPAREN? parse_expr(ts):NULL;
        expect(ts,TK_RPAREN,")");
        st.then_b=parse_block_or_stmt(ts); return st;
    }
    if(peek(ts)->kind==TK_LBRACE){ advance(ts); Block b=parse_braced(ts); Stmt st={0}; st.kind=S_BLOCK; st.then_b=b; return st; }
    if(starts_scalar_type(ts)){
        Type ty=parse_type(ts); Token *nm=peek(ts); expect(ts,TK_IDENT,"name");
        Expr *init=NULL; if(accept(ts,TK_EQ))init=parse_expr(ts); expect(ts,TK_SEMI,";");
        Stmt st={0}; st.kind=S_DECL; st.ty=ty; st.name=strdup(nm->text); st.init=init; return st;
    }
    Expr *e=parse_expr(ts); expect(ts,TK_SEMI,";"); Stmt st={0}; st.kind=S_EXPR; st.expr=e; return st;
}

static void parse_function(TokStream *ts, Program *prog){
    int is_kernel=accept(ts,TK_KW_KERNEL);
    parse_type(ts); /* return type (void) */
    Token *nm=peek(ts); expect(ts,TK_IDENT,"function name"); expect(ts,TK_LPAREN,"(");
    Param *params=NULL; size_t np=0,cap=0;
    while(peek(ts)->kind!=TK_RPAREN){
        Uniformity un=UN_UNIFORM; if(accept(ts,TK_KW_VARYING))un=UN_VARYING; else accept(ts,TK_KW_UNIFORM);
        Type ty=parse_type(ts); Token *pn=peek(ts); expect(ts,TK_IDENT,"param name");
        if(np==cap){cap=cap?cap*2:4;params=realloc(params,cap*sizeof(Param));}
        params[np++]=(Param){strdup(pn->text),ty,un};
        if(!accept(ts,TK_COMMA))break;
    }
    expect(ts,TK_RPAREN,")"); expect(ts,TK_LBRACE,"{"); Block body=parse_braced(ts);
    prog->funcs=realloc(prog->funcs,(prog->nfuncs+1)*sizeof(Function));
    prog->funcs[prog->nfuncs++]=(Function){strdup(nm->text),params,np,body,is_kernel};
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
}
Program parse_program(TokStream *ts){
    Program prog={0};
    while(peek(ts)->kind!=TK_EOF){
        if(peek(ts)->kind==TK_KW_STRUCT){ advance(ts); parse_struct(ts,&prog); } else parse_function(ts,&prog);
    }
    return prog;
}
