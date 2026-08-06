/* lexer.c — hand-written tokenizer for BinC. */
#include "binc.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int g_last_line=0, g_last_col=0, g_err_count=0;
jmp_buf *g_recover=NULL;

_Noreturn void die(int line, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int ln=line?line:g_last_line, col=line?0:g_last_col;
    if(ln&&col) fprintf(stderr,"binc: error (line %d, col %d): ",ln,col);
    else if(ln) fprintf(stderr,"binc: error (line %d): ",ln);
    else fprintf(stderr,"binc: error: ");
    vfprintf(stderr, fmt, ap); fprintf(stderr,"\n"); va_end(ap);
    g_err_count++;
    if(g_recover) longjmp(*g_recover, 1);
    exit(1);
}
int had_errors(void){ return g_err_count>0; }
static char *dup_n(const char *s, size_t n){ char *p=malloc(n+1); memcpy(p,s,n); p[n]='\0'; return p; }
typedef struct { const char *p; int line; const char *line_start; } L;
static Token tk(TokKind k,int ln,int col){ Token t={k,NULL,0,0,ln,col}; return t; }

void lex(const char *src, Token **out, size_t *out_n, int first_line, int hlsl){
    L l={src,first_line,src}; Token *t=NULL; size_t n=0,cap=0;
    #define PUSH(x) do{ if(n==cap){cap=cap?cap*2:64;t=realloc(t,cap*sizeof(Token));} t[n++]=(x);}while(0)
    for(;;){
        for(;;){ while(*l.p&&isspace((unsigned char)*l.p)){ if(*l.p=='\n'){l.line++; l.line_start=l.p+1;} l.p++; }
            if(l.p[0]=='/'&&l.p[1]=='/'){ while(*l.p&&*l.p!='\n')l.p++; }
            else if(l.p[0]=='/'&&l.p[1]=='*'){
                l.p+=2; while(*l.p && !(l.p[0]=='*'&&l.p[1]=='/')){ if(*l.p=='\n'){l.line++; l.line_start=l.p+1;} l.p++; }
                if(!*l.p) die(l.line,"unterminated block comment"); l.p+=2;
            } else break; }
        if(!*l.p){ PUSH(tk(TK_EOF,l.line,0)); break; }
        int ln=l.line; int col=(int)(l.p-l.line_start)+1; char c=*l.p;
        if(c=='('){l.p++;PUSH(tk(TK_LPAREN,ln,col));continue;}
        if(c==')'){l.p++;PUSH(tk(TK_RPAREN,ln,col));continue;}
        if(c=='{'){l.p++;PUSH(tk(TK_LBRACE,ln,col));continue;}
        if(c=='}'){l.p++;PUSH(tk(TK_RBRACE,ln,col));continue;}
        if(c=='['){ if(l.p[1]=='['){l.p+=2;PUSH(tk(TK_DBL_LBRACK,ln,col));} else {l.p++;PUSH(tk(TK_LBRACK,ln,col));} continue;}
        if(c==']'){ if(l.p[1]==']'){l.p+=2;PUSH(tk(TK_DBL_RBRACK,ln,col));} else {l.p++;PUSH(tk(TK_RBRACK,ln,col));} continue;}
        if(c==','){l.p++;PUSH(tk(TK_COMMA,ln,col));continue;}
        if(c==';'){l.p++;PUSH(tk(TK_SEMI,ln,col));continue;}
        if(c=='.'){l.p++;PUSH(tk(TK_DOT,ln,col));continue;}
        if(c=='?'){l.p++;PUSH(tk(TK_QUESTION,ln,col));continue;}
        if(c==':'){l.p++;PUSH(tk(TK_COLON,ln,col));continue;}
        if(c=='+'){l.p++; if(*l.p=='+'){l.p++;PUSH(tk(TK_INC,ln,col));} else if(*l.p=='='){l.p++;PUSH(tk(TK_PLUSEQ,ln,col));}else{PUSH(tk(TK_PLUS,ln,col));} continue;}
        if(c=='-'){l.p++; if(*l.p=='-'){l.p++;PUSH(tk(TK_DEC,ln,col));} else if(*l.p=='='){l.p++;PUSH(tk(TK_MINUSEQ,ln,col));}
                   else if(*l.p=='>'){l.p++;PUSH(tk(TK_ARROW,ln,col));} else {PUSH(tk(TK_MINUS,ln,col));} continue;}
        if(c=='*'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_STAREQ,ln,col));}else{PUSH(tk(TK_STAR,ln,col));} continue;}
        if(c=='/'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_SLASHEQ,ln,col));}else{PUSH(tk(TK_SLASH,ln,col));} continue;}
        if(c=='%'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_MODEQ,ln,col));}else{PUSH(tk(TK_PERCENT,ln,col));} continue;}
        if(c=='!'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_NEQ,ln,col));}else{PUSH(tk(TK_BANG,ln,col));} continue;}
        if(c=='='){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_EQEQ,ln,col));}else{PUSH(tk(TK_EQ,ln,col));} continue;}
        if(c=='<'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_LE,ln,col));}
                   else if(*l.p=='<'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_SHLEQ,ln,col));}else{PUSH(tk(TK_SHL,ln,col));}}
                   else {PUSH(tk(TK_LT,ln,col));} continue;}
        if(c=='>'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_GE,ln,col));}
                   else if(*l.p=='>'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_SHREQ,ln,col));}else{PUSH(tk(TK_SHR,ln,col));}}
                   else {PUSH(tk(TK_GT,ln,col));} continue;}
        if(c=='&'){l.p++; if(*l.p=='&'){l.p++;PUSH(tk(TK_AND,ln,col));}
                   else if(*l.p=='='){l.p++;PUSH(tk(TK_AMPEQ,ln,col));} else {PUSH(tk(TK_AMP,ln,col));} continue;}
        if(c=='|'){l.p++; if(*l.p=='|'){l.p++;PUSH(tk(TK_OR,ln,col));}
                   else if(*l.p=='='){l.p++;PUSH(tk(TK_PIPEEQ,ln,col));} else {PUSH(tk(TK_PIPE,ln,col));} continue;}
        if(c=='^'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_CARETEQ,ln,col));}else{PUSH(tk(TK_CARET,ln,col));} continue;}
        if(c=='~'){l.p++;PUSH(tk(TK_TILDE,ln,col));continue;}
        if(c=='"'){ /* HLSL string literal (attributes, messages): lex as an identifier */
            const char *s=l.p; l.p++;
            while(*l.p&&*l.p!='"'){ if(*l.p=='\\')l.p++; l.p++; }
            if(*l.p)l.p++;
            Token x=tk(TK_IDENT,ln,col); x.text=dup_n(s,(size_t)(l.p-s)); PUSH(x); continue; }
        if(isdigit((unsigned char)c)){
            const char *s=l.p; int isint=1;
            if(l.p[0]=='0'&&(l.p[1]=='x'||l.p[1]=='X')){
                l.p+=2; while(*l.p&&isxdigit((unsigned char)*l.p))l.p++;
                if(*l.p=='u'||*l.p=='U')l.p++;
                char *num=dup_n(s,(size_t)(l.p-s));
                Token x=tk(TK_ICONST,ln,col); x.ival=(long)strtoul(num+2,NULL,16); free(num); PUSH(x); continue;
            }
            while(*l.p&&(isdigit((unsigned char)*l.p)||*l.p=='.')){ if(*l.p=='.')isint=0; l.p++; }
            if(*l.p=='u'||*l.p=='U'){ isint=1; l.p++; }
            if(*l.p=='h'||*l.p=='H'){ isint=0; l.p++; }  /* HLSL half literal (kept as float) */
            else if(*l.p=='l'||*l.p=='L'){ isint=1; l.p++; if(*l.p=='l'||*l.p=='L')l.p++; }  /* HLSL int64 (kept as int32) */
            char *num=dup_n(s,(size_t)(l.p-s));
            if(*l.p=='f'||*l.p=='F'){ isint=0; l.p++; }
            Token x=tk(isint?TK_ICONST:TK_FCONST,ln,col);
            if(isint) x.ival=atol(num); else x.fval=atof(num);
            free(num); PUSH(x); continue;
        }
        if(isalpha((unsigned char)c)||c=='_'){
            const char *s=l.p; while(*l.p&&(isalnum((unsigned char)*l.p)||*l.p=='_'))l.p++;
            size_t len=(size_t)(l.p-s);
            #define KW(w,k) if(strlen(w)==len&&!memcmp(w,s,len)){PUSH(tk(k,ln,col));goto done;}
            #define VW(w,k,n) if(strlen(w)==len&&!memcmp(w,s,len)){Token x=tk(k,ln,col);x.ival=n;PUSH(x);goto done;}
            VW("float2",TK_KW_FLOAT,2) VW("float3",TK_KW_FLOAT,3) VW("float4",TK_KW_FLOAT,4)
            VW("int2",TK_KW_INT,2) VW("int3",TK_KW_INT,3) VW("int4",TK_KW_INT,4)
            VW("uint2",TK_KW_UINT,2) VW("uint3",TK_KW_UINT,3) VW("uint4",TK_KW_UINT,4)
            VW("float2x2",TK_KW_MAT,2) VW("float2x3",TK_KW_MAT,2) VW("float2x4",TK_KW_MAT,2)
            VW("float3x2",TK_KW_MAT,3) VW("float3x3",TK_KW_MAT,3) VW("float3x4",TK_KW_MAT,3)
            VW("float4x2",TK_KW_MAT,4) VW("float4x3",TK_KW_MAT,4) VW("float4x4",TK_KW_MAT,4)
            VW("float1x1",TK_KW_MAT,1) VW("float1x2",TK_KW_MAT,1) VW("float1x3",TK_KW_MAT,1) VW("float1x4",TK_KW_MAT,1)
            VW("float2x1",TK_KW_MAT,2) VW("float3x1",TK_KW_MAT,3) VW("float4x1",TK_KW_MAT,4)
            VW("int2x2",TK_KW_MAT,2) VW("int3x3",TK_KW_MAT,3) VW("int4x4",TK_KW_MAT,4)
            VW("uint2x2",TK_KW_MAT,2) VW("uint3x3",TK_KW_MAT,3) VW("uint4x4",TK_KW_MAT,4)
            VW("bool2x2",TK_KW_MAT,2) VW("bool3x3",TK_KW_MAT,3) VW("bool4x4",TK_KW_MAT,4)
            VW("bool1x1",TK_KW_MAT,1) VW("bool2x1",TK_KW_MAT,2) VW("bool3x1",TK_KW_MAT,3) VW("bool4x1",TK_KW_MAT,4)
            KW("struct",TK_KW_STRUCT) KW("kernel",TK_KW_KERNEL) KW("void",TK_KW_VOID)
            KW("float",TK_KW_FLOAT) KW("half",TK_KW_HALF) KW("int",TK_KW_INT) KW("uint",TK_KW_UINT) KW("bool",TK_KW_BOOL)
            KW("return",TK_KW_RETURN) KW("if",TK_KW_IF) KW("else",TK_KW_ELSE) KW("for",TK_KW_FOR) KW("while",TK_KW_WHILE) KW("do",TK_KW_DO)
            KW("break",TK_KW_BREAK) KW("continue",TK_KW_CONTINUE)
            KW("switch",TK_KW_SWITCH) KW("case",TK_KW_CASE) KW("default",TK_KW_DEFAULT)
            KW("true",TK_KW_TRUE) KW("false",TK_KW_FALSE)
            KW("device",TK_KW_DEVICE) KW("constant",TK_KW_CONSTANT) KW("const",TK_KW_CONSTANT) KW("threadgroup",TK_KW_THREADGROUP)
            KW("thread",TK_KW_THREAD) KW("uniform",TK_KW_UNIFORM) KW("varying",TK_KW_VARYING)
            VW("coord1D",TK_KW_COORD,1) VW("coord2D",TK_KW_COORD,2) VW("coord3D",TK_KW_COORD,3)
            VW("mat2",TK_KW_MAT,2) VW("mat3",TK_KW_MAT,3) VW("mat4",TK_KW_MAT,4)
            KW("grid_extent",TK_KW_GRID_EXTENT) KW("atomic",TK_KW_ATOMIC)
            KW("vertex",TK_KW_VERTEX) KW("fragment",TK_KW_FRAGMENT) KW("vertex_id",TK_KW_VERTEX_ID)
            KW("texture2d",TK_KW_TEXTURE2D) KW("sampler",TK_KW_SAMPLER)
            KW("template",TK_KW_TEMPLATE) KW("typename",TK_KW_TYPENAME)
            /* HLSL-only surface (mode-gated: `out`/`in` are legal BinC identifiers) */
            #define HLSLKW(w,k) if(hlsl&&strlen(w)==len&&!memcmp(w,s,len)){PUSH(tk(k,ln,col));goto done;}
            HLSLKW("cbuffer",TK_KW_CBUFFER) HLSLKW("tbuffer",TK_KW_TBUFFER) HLSLKW("groupshared",TK_KW_GROUPSHARED)
            HLSLKW("in",TK_KW_IN) HLSLKW("out",TK_KW_OUT) HLSLKW("inout",TK_KW_INOUT) HLSLKW("static",TK_KW_STATIC)
            HLSLKW("register",TK_KW_REGISTER) HLSLKW("packoffset",TK_KW_PACKOFFSET)
            HLSLKW("linear",TK_KW_LINEAR) HLSLKW("noperspective",TK_KW_NOPERSPECTIVE)
            HLSLKW("centroid",TK_KW_CENTROID) HLSLKW("sample",TK_KW_SAMPLE)
            HLSLKW("Texture2D",TK_KW_TEXTURE2D) HLSLKW("RWTexture2D",TK_KW_RWTEXTURE2D)
            HLSLKW("SamplerState",TK_KW_SAMPLER) HLSLKW("SamplerComparisonState",TK_KW_SAMPLER)
            HLSLKW("StructuredBuffer",TK_KW_STRUCTURED) HLSLKW("RWStructuredBuffer",TK_KW_RWSTRUCTURED)
            HLSLKW("ByteAddressBuffer",TK_KW_BYTEADDR) HLSLKW("RWByteAddressBuffer",TK_KW_RWBYTEADDR)
            HLSLKW("Buffer",TK_KW_STRUCTURED) HLSLKW("RWBuffer",TK_KW_RWSTRUCTURED)
            #undef HLSLKW
            #undef KW
            Token x=tk(TK_IDENT,ln,col); x.text=dup_n(s,len); PUSH(x);
            done:; continue;
        }
        die(ln,"unexpected character '%c'",c);
    }
    *out=t; *out_n=n;
    #undef PUSH
    #undef VW
}
