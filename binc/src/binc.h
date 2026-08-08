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
               T_COORD, T_GRID_EXTENT, T_ATOMIC, T_TEXTURE, T_SAMPLER, T_TVAR } TypeKind;

typedef struct {
    TypeKind kind;
    char    *struct_name;
    char    *tvar;   /* template type variable name, or NULL */
    int      is_ptr;
    AddrSpace as;
    int      vecn; /* 0/1 = scalar; 2/3/4 = vector width */
    int      matn; /* 2/3/4 = column-major float matrix; 0 = not a matrix */
    int      matm; /* 0 = square; else non-square column count (float4x3: matn=4, matm=3) */
    int      coordn; /* T_COORD dimensionality: 1, 2, or 3 */
    TypeKind atomic_base; /* T_ATOMIC's scalar payload */
    TypeKind tex_elt; /* T_TEXTURE's texel scalar type */
    int      tex_cube; /* T_TEXTURE: cube map (sample with float3 direction) */
    int      array_n, array_m; /* threadgroup array extents, if declared */
} Type;

/* runtime value classification for type-directed codegen */
typedef enum { VK_F32, VK_I32, VK_U32, VK_I1, VK_PTR } ValKind;

/* ---- AST ---- */
typedef struct Expr Expr;
typedef enum {
    E_FCONST, E_ICONST, E_BOOL, E_IDENT, E_DEREF, E_FIELD, E_INDEX,
    E_BIN, E_CMP, E_LOG, E_NOT, E_NEG, E_COMPL, E_ASSIGN, E_CALL, E_CAST, E_TERNARY, E_INCDEC
} ExprKind;

typedef enum { B_ADD, B_SUB, B_MUL, B_DIV, B_MOD, B_AND, B_OR, B_XOR, B_SHL, B_SHR } BinOp;
typedef enum { C_EQ, C_NE, C_LT, C_LE, C_GT, C_GE } CmpOp;
typedef enum { L_AND, L_OR } LogOp;
typedef enum { A_ASSIGN, A_ADDEQ, A_SUBEQ, A_MULEQ, A_DIVEQ, A_MODEQ,
               A_ANDEQ, A_OREQ, A_XOREQ, A_SHLEQ, A_SHREQ } AssignOp;

struct Expr {
    ExprKind kind;
    int line, col;
    double   fval; long ival; int bval;
    char    *name, *field;
    BinOp    bop; CmpOp cmp; LogOp log; AssignOp aop;
    Type     cty; /* E_CAST target type */
    Expr    *operand, *lhs, *rhs;
    Expr   **args; size_t nargs; /* E_CALL */
    Expr   *callee; /* non-identifier call target, currently atomic methods */
};

typedef struct Stmt Stmt;
typedef struct { Stmt *stmts; size_t n; } Block;
typedef struct { Expr *val; Block body; } SCase;
typedef enum { S_EXPR, S_DECL, S_RETURN, S_IF, S_FOR, S_WHILE, S_DOWHILE, S_SWITCH, S_BLOCK, S_BREAK, S_CONTINUE } StmtKind;
struct Stmt {
    StmtKind kind;
    int line, col;
    Expr *expr;
    /* S_DECL */ Type ty; char *name; Expr *init; int is_const;
    /* S_IF / S_WHILE / S_DOWHILE / S_SWITCH */ Expr *cond;
    /* bodies: S_IF.then_b/else_b, S_WHILE.then_b, S_DOWHILE.then_b, S_FOR.then_b, S_BLOCK.then_b */
    Block then_b, else_b;
    /* S_FOR */ Stmt *for_init; Expr *for_cond, *for_incr;
    /* S_SWITCH */ Expr *sw_cond; SCase *cases; size_t ncases; int has_default; Block def_body;
};

typedef struct { char *name; Type ty; Uniformity un; Expr *def; } Param;
typedef enum { ST_NONE, ST_VERTEX, ST_FRAGMENT, ST_GEOMETRY } Stage;
typedef struct { char *name; char *link_name; Param *params; size_t nparams; Block body; int is_kernel; Stage stage; Type ret; int line;
    int is_template; char *tvar; Type tvar_ty; } Function;
typedef struct { char *name; Type ty; int attr; int attr_idx; char *sem; } Field; /* attr: 0 none, 1 position, 2 flat, 3 color(N), 4 depth, 5 user(locnN); sem: raw HLSL semantic */
typedef struct { char *tag; Field *fields; size_t nfields; int is_template; char *tvar; } StructDef;
typedef struct { char *name; Type ty; int is_int; long ival; double fval; int line; int mut; } ConstDef;
typedef struct { StructDef *structs; size_t nstructs; Function *funcs; size_t nfuncs; ConstDef *consts; size_t nconsts; } Program;

/* ---- lexer ---- */
typedef enum {
    TK_EOF, TK_IDENT, TK_FCONST, TK_ICONST,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACK, TK_RBRACK, TK_DBL_LBRACK, TK_DBL_RBRACK,
    TK_STAR, TK_COMMA, TK_SEMI, TK_ARROW, TK_DOT,
    TK_PLUS, TK_MINUS, TK_SLASH, TK_PERCENT, TK_BANG,
    TK_EQ, TK_EQEQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE, TK_AND, TK_OR,    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SHR,
    TK_QUESTION, TK_COLON, TK_INC, TK_DEC,
    TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_MODEQ,
    TK_AMPEQ, TK_PIPEEQ, TK_CARETEQ, TK_SHLEQ, TK_SHREQ,
    TK_KW_STRUCT, TK_KW_KERNEL, TK_KW_VOID, TK_KW_FLOAT, TK_KW_HALF, TK_KW_INT, TK_KW_UINT, TK_KW_BOOL,
    TK_KW_RETURN, TK_KW_IF, TK_KW_ELSE, TK_KW_FOR, TK_KW_WHILE, TK_KW_DO, TK_KW_TRUE, TK_KW_FALSE,
    TK_KW_BREAK, TK_KW_CONTINUE, TK_KW_SWITCH, TK_KW_CASE, TK_KW_DEFAULT,
    TK_KW_DEVICE, TK_KW_CONSTANT, TK_KW_THREADGROUP, TK_KW_THREAD, TK_KW_UNIFORM, TK_KW_VARYING,
    TK_KW_COORD, TK_KW_GRID_EXTENT, TK_KW_ATOMIC, TK_KW_VERTEX, TK_KW_FRAGMENT, TK_KW_VERTEX_ID,
    TK_KW_MAT, TK_KW_TEXTURE2D, TK_KW_SAMPLER, TK_KW_TEMPLATE, TK_KW_TYPENAME,
    /* HLSL frontend keywords */
    TK_KW_CBUFFER, TK_KW_TBUFFER, TK_KW_GROUPSHARED, TK_KW_IN, TK_KW_OUT, TK_KW_INOUT, TK_KW_STATIC,
    TK_KW_REGISTER, TK_KW_PACKOFFSET, TK_KW_RWTEXTURE2D,
    TK_KW_STRUCTURED, TK_KW_RWSTRUCTURED, TK_KW_BYTEADDR, TK_KW_RWBYTEADDR,
    TK_KW_LINEAR, TK_KW_NOPERSPECTIVE, TK_KW_CENTROID, TK_KW_SAMPLE,
    TK_KW_TECHNIQUE, TK_KW_PASS,
} TokKind;
typedef struct { TokKind kind; char *text; double fval; long ival; int line, col; } Token;
typedef struct { Token *toks; size_t n; size_t i; } TokStream;

void lex(const char *src, Token **out, size_t *out_n, int first_line, int hlsl);
_Noreturn void die(int line, const char *fmt, ...);
int had_errors(void);
/* position of the expression/statement being lowered, for located die(0, ...) errors */
extern int g_last_line, g_last_col, g_err_count;
/* when set, die() reports and longjmps to the recovery point instead of exiting */
extern jmp_buf *g_recover;
extern int g_uses_discard;
extern int g_uses_bitintrin;
Program parse_program(TokStream *ts);
/* ---- shared parser API (also used by the HLSL frontend) ---- */
Token *peek(TokStream *ts); Token *advance(TokStream *ts);
int accept(TokStream *ts, TokKind k); void expect(TokStream *ts, TokKind k, const char *w);
void expect_name(TokStream *ts, const char *what);
Expr *E(ExprKind k, int line, int col);
Type parse_type(TokStream *ts); int starts_scalar_type(TokStream *ts); int parse_array_extent(TokStream *ts);
Expr *parse_expr(TokStream *ts); Stmt parse_stmt(TokStream *ts);
Block parse_braced(TokStream *ts); Block parse_block_or_stmt(TokStream *ts);
void blk_push(Block *b, Stmt s); int is_stag(const char *s);
void parse_struct(TokStream *ts, Program *prog); void parse_function(TokStream *ts, Program *prog);
extern const char *g_tvar; extern Program *g_parse_prog;
/* ---- HLSL frontend (Phases 1-7) ---- */
typedef struct { char *name; char *sem; Type ty; int inq; int reg; int is_uniform; Expr *def; } HLSLParam; /* inq: 0 plain, 1 in, 2 out, 3 inout */
typedef struct { char *name; HLSLParam *params; size_t np; Type ret; char *ret_sem;
    Block body; int line; int numtx, numty, numtz; int has_numthreads; int is_export; } HLSLFunc;
typedef struct { char *tag; Field *fields; size_t nfields; } HLSLStruct;
typedef struct { char *name; Field *fields; size_t nfields; int reg; } HLCBuf;
typedef struct { char *name; Type ty; int is_groupshared; int is_const; int is_static; int is_int;
    long ival; double fval; int has_init; int line; int reg;
    char *tex_name; /* sampler_state { Texture = <X>; } — the bound texture */ } HLSLGlobal;
typedef struct { HLSLFunc *funcs; size_t nfuncs; HLSLStruct *structs; size_t nstructs;
    HLCBuf *cbufs; size_t ncbufs; HLSLGlobal *globals; size_t nglobals; } HLSLProg;
HLSLProg hlsl_parse(TokStream *ts);
Program hlsl_build(HLSLProg *hp, const char *entry, const char *profile, int stage_all);
void emit_air(FILE *out, Program *prog);
void binc_set_air(const char *triple, int sdk, int minor);
void binc_reflect(FILE *out, HLSLProg *hp, Program *prog);
void interp_run(const Program *prog);
#endif
