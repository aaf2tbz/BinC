/* binc.h — shared definitions for the bootstrap BinC compiler.
 * BinC: "works as C, acts as Metal." Reads .binc, emits AIR .ll, drives metal/metallib.
 */
#ifndef BINC_H
#define BINC_H
#include <stddef.h>
#include <stdio.h>
#include <setjmp.h>

typedef enum { AS_DEVICE=1, AS_CONSTANT=2, AS_THREADGROUP=3, AS_THREAD=0 } AddrSpace;
typedef enum { UN_UNIFORM=0, UN_VARYING=1 } Uniformity;

typedef enum { T_VOID, T_FLOAT, T_HALF, T_INT32, T_UINT32, T_BOOL, T_STRUCT,
               T_COORD, T_GRID_EXTENT, T_ATOMIC } TypeKind;

typedef struct {
    TypeKind kind;
    char    *struct_name;
    int      is_ptr;
    AddrSpace as;
    int      vecn; /* 0/1 = scalar; 2/3/4 = vector width */
    int      coordn; /* T_COORD dimensionality: 1, 2, or 3 */
    TypeKind atomic_base; /* T_ATOMIC's scalar payload */
    int      array_n, array_m; /* threadgroup array extents, if declared */
} Type;

/* runtime value classification for type-directed codegen */
typedef enum { VK_F32, VK_I32, VK_U32, VK_I1, VK_PTR } ValKind;

/* ---- AST ---- */
typedef struct Expr Expr;
typedef enum {
    E_FCONST, E_ICONST, E_BOOL, E_IDENT, E_DEREF, E_FIELD, E_INDEX,
    E_BIN, E_CMP, E_LOG, E_NOT, E_NEG, E_ASSIGN, E_CALL
} ExprKind;

typedef enum { B_ADD, B_SUB, B_MUL, B_DIV, B_MOD } BinOp;
typedef enum { C_EQ, C_NE, C_LT, C_LE, C_GT, C_GE } CmpOp;
typedef enum { L_AND, L_OR } LogOp;
typedef enum { A_ASSIGN, A_ADDEQ, A_SUBEQ, A_MULEQ, A_DIVEQ, A_MODEQ } AssignOp;

struct Expr {
    ExprKind kind;
    int line, col;
    double   fval; long ival; int bval;
    char    *name, *field;
    BinOp    bop; CmpOp cmp; LogOp log; AssignOp aop;
    Expr    *operand, *lhs, *rhs;
    Expr   **args; size_t nargs; /* E_CALL */
    Expr   *callee; /* non-identifier call target, currently atomic methods */
};

typedef struct Stmt Stmt;
typedef struct { Stmt *stmts; size_t n; } Block;
typedef enum { S_EXPR, S_DECL, S_RETURN, S_IF, S_FOR, S_WHILE, S_BLOCK, S_BREAK, S_CONTINUE } StmtKind;
struct Stmt {
    StmtKind kind;
    int line, col;
    Expr *expr;
    /* S_DECL */ Type ty; char *name; Expr *init;
    /* S_IF / S_WHILE */ Expr *cond;
    /* bodies: S_IF.then_b/else_b, S_WHILE.then_b, S_FOR.then_b, S_BLOCK.then_b */
    Block then_b, else_b;
    /* S_FOR */ Stmt *for_init; Expr *for_cond, *for_incr;
};

typedef struct { char *name; Type ty; Uniformity un; } Param;
typedef enum { ST_NONE, ST_VERTEX, ST_FRAGMENT } Stage;
typedef struct { char *name; Param *params; size_t nparams; Block body; int is_kernel; Stage stage; Type ret; int line; } Function;
typedef struct { char *name; Type ty; } Field;
typedef struct { char *tag; Field *fields; size_t nfields; } StructDef;
typedef struct { StructDef *structs; size_t nstructs; Function *funcs; size_t nfuncs; } Program;

/* ---- lexer ---- */
typedef enum {
    TK_EOF, TK_IDENT, TK_FCONST, TK_ICONST,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACK, TK_RBRACK,
    TK_STAR, TK_COMMA, TK_SEMI, TK_ARROW, TK_DOT,
    TK_PLUS, TK_MINUS, TK_SLASH, TK_PERCENT, TK_BANG,
    TK_EQ, TK_EQEQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE, TK_AND, TK_OR,
    TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_MODEQ,
    TK_KW_STRUCT, TK_KW_KERNEL, TK_KW_VOID, TK_KW_FLOAT, TK_KW_HALF, TK_KW_INT, TK_KW_UINT, TK_KW_BOOL,
    TK_KW_RETURN, TK_KW_IF, TK_KW_ELSE, TK_KW_FOR, TK_KW_WHILE, TK_KW_TRUE, TK_KW_FALSE,
    TK_KW_BREAK, TK_KW_CONTINUE,
    TK_KW_DEVICE, TK_KW_CONSTANT, TK_KW_THREADGROUP, TK_KW_THREAD, TK_KW_UNIFORM, TK_KW_VARYING,
    TK_KW_COORD, TK_KW_GRID_EXTENT, TK_KW_ATOMIC, TK_KW_VERTEX, TK_KW_FRAGMENT, TK_KW_VERTEX_ID
} TokKind;
typedef struct { TokKind kind; char *text; double fval; long ival; int line, col; } Token;
typedef struct { Token *toks; size_t n; size_t i; } TokStream;

void lex(const char *src, Token **out, size_t *out_n);
_Noreturn void die(int line, const char *fmt, ...);
int had_errors(void);
/* position of the expression/statement being lowered, for located die(0, ...) errors */
extern int g_last_line, g_last_col, g_err_count;
/* when set, die() reports and longjmps to the recovery point instead of exiting */
extern jmp_buf *g_recover;
Program parse_program(TokStream *ts);
void emit_air(FILE *out, const Program *prog);
#endif
