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

void lex(const char *src, Token **out, size_t *out_n){
    L l={src,1,src}; Token *t=NULL; size_t n=0,cap=0;
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
        if(c=='['){l.p++;PUSH(tk(TK_LBRACK,ln,col));continue;}
        if(c==']'){l.p++;PUSH(tk(TK_RBRACK,ln,col));continue;}
        if(c==','){l.p++;PUSH(tk(TK_COMMA,ln,col));continue;}
        if(c==';'){l.p++;PUSH(tk(TK_SEMI,ln,col));continue;}
        if(c=='.'){l.p++;PUSH(tk(TK_DOT,ln,col));continue;}
        if(c=='+'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_PLUSEQ,ln,col));}else{PUSH(tk(TK_PLUS,ln,col));} continue;}
        if(c=='-'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_MINUSEQ,ln,col));}
                   else if(*l.p=='>'){l.p++;PUSH(tk(TK_ARROW,ln,col));} else {PUSH(tk(TK_MINUS,ln,col));} continue;}
        if(c=='*'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_STAREQ,ln,col));}else{PUSH(tk(TK_STAR,ln,col));} continue;}
        if(c=='/'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_SLASHEQ,ln,col));}else{PUSH(tk(TK_SLASH,ln,col));} continue;}
        if(c=='%'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_MODEQ,ln,col));}else{PUSH(tk(TK_PERCENT,ln,col));} continue;}
        if(c=='!'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_NEQ,ln,col));}else{PUSH(tk(TK_BANG,ln,col));} continue;}
        if(c=='='){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_EQEQ,ln,col));}else{PUSH(tk(TK_EQ,ln,col));} continue;}
        if(c=='<'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_LE,ln,col));}else{PUSH(tk(TK_LT,ln,col));} continue;}
        if(c=='>'){l.p++; if(*l.p=='='){l.p++;PUSH(tk(TK_GE,ln,col));}else{PUSH(tk(TK_GT,ln,col));} continue;}
        if(c=='&'){l.p++; if(*l.p=='&'){l.p++;PUSH(tk(TK_AND,ln,col));}else die(ln,"unexpected '&'"); continue;}
        if(c=='|'){l.p++; if(*l.p=='|'){l.p++;PUSH(tk(TK_OR,ln,col));}else die(ln,"unexpected '|'"); continue;}
        if(isdigit((unsigned char)c)){
            const char *s=l.p; int isint=1;
            while(*l.p&&(isdigit((unsigned char)*l.p)||*l.p=='.')){ if(*l.p=='.')isint=0; l.p++; }
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
            KW("struct",TK_KW_STRUCT) KW("kernel",TK_KW_KERNEL) KW("void",TK_KW_VOID)
            KW("float",TK_KW_FLOAT) KW("half",TK_KW_HALF) KW("int",TK_KW_INT) KW("uint",TK_KW_UINT) KW("bool",TK_KW_BOOL)
            KW("return",TK_KW_RETURN) KW("if",TK_KW_IF) KW("else",TK_KW_ELSE) KW("for",TK_KW_FOR) KW("while",TK_KW_WHILE)
            KW("break",TK_KW_BREAK) KW("continue",TK_KW_CONTINUE)
            KW("true",TK_KW_TRUE) KW("false",TK_KW_FALSE)
            KW("device",TK_KW_DEVICE) KW("constant",TK_KW_CONSTANT) KW("threadgroup",TK_KW_THREADGROUP)
            KW("thread",TK_KW_THREAD) KW("uniform",TK_KW_UNIFORM) KW("varying",TK_KW_VARYING)
            VW("coord1D",TK_KW_COORD,1) VW("coord2D",TK_KW_COORD,2) VW("coord3D",TK_KW_COORD,3)
            KW("grid_extent",TK_KW_GRID_EXTENT) KW("atomic",TK_KW_ATOMIC)
            KW("vertex",TK_KW_VERTEX) KW("fragment",TK_KW_FRAGMENT) KW("vertex_id",TK_KW_VERTEX_ID)
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
