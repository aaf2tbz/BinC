/* parser.c — recursive-descent parser for BinC. */
#include "binc.h"
#include <stdlib.h>
#include <string.h>

Token *peek(TokStream *ts){ return &ts->toks[ts->i]; }
Token *advance(TokStream *ts){ return &ts->toks[ts->i++]; }
int accept(TokStream *ts,TokKind k){ if(peek(ts)->kind==k){ts->i++;return 1;} return 0; }
void expect(TokStream *ts,TokKind k,const char *w){ if(peek(ts)->kind!=k) die(peek(ts)->line,"expected %s",w); ts->i++; }

/* active template type parameter name while parsing a template function signature+body */
const char *g_tvar = NULL;
/* Program under construction, so parse_type can instantiate template structs */
Program *g_parse_prog = NULL;
/* known struct tags, for disambiguating `Dog d;` local declarations */
static char **stags=NULL; static size_t nstags=0;

/* "<tag>$<key>" for template struct instantiations; scalar/vector/matrix/struct keys */
static void st_key(const Type *ty, char *buf, size_t n){
    if(ty->kind==T_STRUCT) snprintf(buf,n,"s%s",ty->struct_name);
    else if(ty->matn) snprintf(buf,n,"m%d",ty->matn);
    else if(ty->vecn>1) snprintf(buf,n,"v%d%s",ty->vecn,ty->kind==T_HALF?"f16":"f32");
    else snprintf(buf,n,"%s",ty->kind==T_INT32?"i32":ty->kind==T_UINT32?"u32":
        ty->kind==T_FLOAT?"f32":ty->kind==T_HALF?"f16":ty->kind==T_BOOL?"b1":"?");
}
static StructDef *inst_struct(Program *prog, StructDef *tpl, const Type *concrete){
    char key[64]; st_key(concrete,key,sizeof key);
    char tag[96]; snprintf(tag,sizeof tag,"%s$%s",tpl->tag,key);
    for(size_t i=0;i<prog->nstructs;i++) if(!strcmp(prog->structs[i].tag,tag)) return &prog->structs[i];
    StructDef inst=*tpl;
    inst.tag=strdup(tag);
    inst.is_template=0; inst.tvar=NULL;
    inst.fields=calloc(tpl->nfields,sizeof(Field));
    for(size_t i=0;i<tpl->nfields;i++){
        inst.fields[i].name=strdup(tpl->fields[i].name);
        inst.fields[i].attr=tpl->fields[i].attr; inst.fields[i].attr_idx=tpl->fields[i].attr_idx;
        inst.fields[i].ty=tpl->fields[i].ty;
        if(inst.fields[i].ty.tvar) inst.fields[i].ty=*concrete;
        inst.fields[i].ty.tvar=NULL;
    }
    prog->structs=realloc(prog->structs,(prog->nstructs+1)*sizeof(StructDef));
    prog->structs[prog->nstructs++]=inst;
    stags=realloc(stags,(nstags+1)*sizeof(char*)); stags[nstags++]=strdup(tag);
    return &prog->structs[prog->nstructs-1];
}

Type parse_type(TokStream *ts){
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
    else if(accept(ts,TK_KW_MAT)){ t.kind=T_FLOAT; t.matn=(int)pt->ival; }
    else if(peek(ts)->kind==TK_IDENT){
        /* HLSL template-style type spellings + template-arg struct fallback */
        const char *txt=peek(ts)->text;
        int has_lt = (ts->i+1<ts->n) && ts->toks[ts->i+1].kind==TK_LT;
        if(has_lt&&!strcmp(txt,"matrix")){
            advance(ts); expect(ts,TK_LT,"<");
            parse_type(ts); /* element type ignored */
            expect(ts,TK_COMMA,","); Token *rn=peek(ts); expect(ts,TK_ICONST,"matrix rows");
            expect(ts,TK_COMMA,","); Token *cn=peek(ts); expect(ts,TK_ICONST,"matrix cols");
            expect(ts,TK_GT,">");
            t.kind=T_FLOAT; t.matn=(int)(rn->ival>cn->ival?rn->ival:cn->ival); return t;
        }
        if(has_lt&&!strcmp(txt,"vector")){
            advance(ts); expect(ts,TK_LT,"<");
            Type et=parse_type(ts);
            expect(ts,TK_COMMA,","); Token *vn=peek(ts); expect(ts,TK_ICONST,"vector size");
            expect(ts,TK_GT,">");
            et.vecn=(int)vn->ival; if(et.vecn<2)et.vecn=1; return et;
        }
        if(has_lt&&(!strcmp(txt,"ConstantBuffer")||!strcmp(txt,"TextureBuffer"))){
            advance(ts); expect(ts,TK_LT,"<");
            Type et=parse_type(ts);
            expect(ts,TK_GT,">");
            et.is_ptr=1; et.as=AS_CONSTANT; et.array_n=0; et.array_m=0; return et;
        }
        if(has_lt){
            /* template struct instantiation (Pair<float>) or a generic
             * templated type (InputPatch<T,N>, OutputPatch<T,N>, ...) */
            StructDef *tpl=NULL;
            if(g_parse_prog) for(size_t i=0;i<g_parse_prog->nstructs;i++)
                if(!strcmp(g_parse_prog->structs[i].tag,txt)&&g_parse_prog->structs[i].is_template){ tpl=&g_parse_prog->structs[i]; break; }
            if(tpl){
                advance(ts); expect(ts,TK_LT,"<");
                Type inner=parse_type(ts); expect(ts,TK_GT,">");
                if(inner.kind==T_TVAR||inner.tvar) die(peek(ts)->line,"template struct instantiation with a type variable is not supported");
                if(inner.is_ptr) die(peek(ts)->line,"template struct instantiation with a pointer type is not supported");
                StructDef *sd=inst_struct(g_parse_prog,tpl,&inner);
                t.kind=T_STRUCT; t.struct_name=strdup(sd->tag);
                return t;
            }
            /* generic templated type: consume <...> and treat as a struct */
            advance(ts); expect(ts,TK_LT,"<");
            parse_type(ts);
            while(accept(ts,TK_COMMA)){ if(peek(ts)->kind==TK_ICONST) advance(ts); else parse_type(ts); }
            expect(ts,TK_GT,">");
            t.kind=T_STRUCT; t.struct_name=strdup(txt); t.is_ptr=0; return t;
        }
        if(!strcmp(txt,"min16float")||!strcmp(txt,"min10float")){ advance(ts); t.kind=T_FLOAT; return t; }
        if(!strcmp(txt,"matrix")){ advance(ts); t.kind=T_FLOAT; t.matn=4; return t; } /* bare matrix == float4x4 */
        if(!strcmp(txt,"vector")){ advance(ts); t.kind=T_FLOAT; t.vecn=4; return t; } /* bare vector == float4 */
        if(!strcmp(txt,"min16int")||!strcmp(txt,"min16uint")){ advance(ts); t.kind=!strcmp(txt,"min16int")?T_INT32:T_UINT32; return t; }
        if(!strcmp(txt,"int64_t")||!strcmp(txt,"int16_t")||!strcmp(txt,"int8_t")){ advance(ts); t.kind=T_INT32; return t; }
        if(!strcmp(txt,"uint64_t")||!strcmp(txt,"uint16_t")||!strcmp(txt,"uint8_t")){ advance(ts); t.kind=T_UINT32; return t; }
        if(!strcmp(txt,"dword")){ advance(ts); t.kind=T_UINT32; return t; } /* HLSL dword == uint32 */
        if(!strcmp(txt,"word")){ advance(ts); t.kind=T_UINT32; return t; }
        if(!strcmp(txt,"double")){ advance(ts); t.kind=T_FLOAT; return t; } /* double -> float for now */
        if(!strncmp(txt,"half",4)&&txt[4]>='1'&&txt[4]<='4'&&txt[5]=='\0'){
            advance(ts); t.kind=T_HALF; t.vecn=txt[4]-'0'; return t; }
        /* struct types */
        if(g_tvar && !strcmp(txt,g_tvar)){ advance(ts); t.kind=T_TVAR; t.tvar=strdup(g_tvar); }
        else {
            char *tag=strdup(advance(ts)->text);
            if(accept(ts,TK_LT)){
                /* template struct instantiation: Pair<float> */
                Type inner=parse_type(ts); expect(ts,TK_GT,">");
                StructDef *tpl=NULL;
                if(g_parse_prog) for(size_t i=0;i<g_parse_prog->nstructs;i++)
                    if(!strcmp(g_parse_prog->structs[i].tag,tag)&&g_parse_prog->structs[i].is_template){ tpl=&g_parse_prog->structs[i]; break; }
                if(!tpl) die(peek(ts)->line,"%s is not a template struct",tag);
                if(inner.kind==T_TVAR||inner.tvar) die(peek(ts)->line,"template struct instantiation with a type variable is not supported");
                if(inner.is_ptr) die(peek(ts)->line,"template struct instantiation with a pointer type is not supported");
                StructDef *sd=inst_struct(g_parse_prog,tpl,&inner);
                t.kind=T_STRUCT; t.struct_name=strdup(sd->tag);
            } else { t.kind=T_STRUCT; t.struct_name=tag; }
        }
    }
    else if(accept(ts,TK_KW_TEXTURE2D)){
        t.kind=T_TEXTURE;
        if(accept(ts,TK_LT)){
            Token *et=peek(ts);
            if(!(et->kind==TK_IDENT||et->kind==TK_KW_FLOAT||et->kind==TK_KW_HALF||et->kind==TK_KW_INT||et->kind==TK_KW_UINT))
                die(et->line,"expected a texture element type");
            advance(ts);
            if(et->kind==TK_KW_FLOAT) t.tex_elt=T_FLOAT;
            else if(et->kind==TK_KW_HALF) t.tex_elt=T_HALF;
            else if(et->kind==TK_KW_INT) t.tex_elt=T_INT32;
            else if(et->kind==TK_KW_UINT) t.tex_elt=T_UINT32;
            else if(et->kind==TK_IDENT&&!strcmp(et->text,"float")) t.tex_elt=T_FLOAT;
            else die(et->line,"unsupported texture element type %s (use float, half, int, uint)",et->kind==TK_IDENT?et->text:"?");
            expect(ts,TK_GT,">");
        } else t.tex_elt=T_FLOAT; /* bare Texture2D defaults to float */
    }
    else if(accept(ts,TK_KW_SAMPLER)) t.kind=T_SAMPLER;
    else if(peek(ts)->kind==TK_IDENT){
        if(g_tvar && !strcmp(peek(ts)->text,g_tvar)){ advance(ts); t.kind=T_TVAR; t.tvar=strdup(g_tvar); }
        else {
            char *tag=strdup(advance(ts)->text);
            if(accept(ts,TK_LT)){
                /* template struct instantiation: Pair<float> */
                Type inner=parse_type(ts); expect(ts,TK_GT,">");
                StructDef *tpl=NULL;
                if(g_parse_prog) for(size_t i=0;i<g_parse_prog->nstructs;i++)
                    if(!strcmp(g_parse_prog->structs[i].tag,tag)&&g_parse_prog->structs[i].is_template){ tpl=&g_parse_prog->structs[i]; break; }
                if(!tpl) die(peek(ts)->line,"%s is not a template struct",tag);
                if(inner.kind==T_TVAR||inner.tvar) die(peek(ts)->line,"template struct instantiation with a type variable is not supported");
                if(inner.is_ptr) die(peek(ts)->line,"template struct instantiation with a pointer type is not supported");
                StructDef *sd=inst_struct(g_parse_prog,tpl,&inner);
                t.kind=T_STRUCT; t.struct_name=strdup(sd->tag);
            } else { t.kind=T_STRUCT; t.struct_name=tag; }
        }
    }
    else die(peek(ts)->line,"expected a type");
    if(accept(ts,TK_STAR)){ t.is_ptr=1; if(t.as==0)t.as=AS_DEVICE; }
    return t;
}
int starts_scalar_type(TokStream *ts){ TokKind k=peek(ts)->kind;
    if(k==TK_IDENT){ /* HLSL template-style type spellings */
        const char *txt=peek(ts)->text;
        if((!strcmp(txt,"matrix")||!strcmp(txt,"vector")||!strcmp(txt,"ConstantBuffer")||!strcmp(txt,"TextureBuffer"))&&
           (ts->i+1<ts->n)&&ts->toks[ts->i+1].kind==TK_LT) return 1;
        if(!strcmp(txt,"min16float")||!strcmp(txt,"min10float")||!strcmp(txt,"min16int")||!strcmp(txt,"min16uint")) return 1;
        if(!strcmp(txt,"int64_t")||!strcmp(txt,"int16_t")||!strcmp(txt,"int8_t")) return 1;
        if(!strcmp(txt,"uint64_t")||!strcmp(txt,"uint16_t")||!strcmp(txt,"uint8_t")||
           !strcmp(txt,"dword")||!strcmp(txt,"word")||!strcmp(txt,"double")||
           !strncmp(txt,"half",4)) return 1;
    }
    return k==TK_KW_FLOAT||k==TK_KW_HALF||k==TK_KW_INT||k==TK_KW_UINT||k==TK_KW_BOOL||
           k==TK_KW_COORD||k==TK_KW_GRID_EXTENT||k==TK_KW_MAT||
           k==TK_KW_TEXTURE2D||k==TK_KW_RWTEXTURE2D||k==TK_KW_SAMPLER||
           k==TK_KW_STRUCTURED||k==TK_KW_RWSTRUCTURED||k==TK_KW_BYTEADDR||k==TK_KW_RWBYTEADDR; }

Expr *E(ExprKind k,int line,int col){ Expr *e=calloc(1,sizeof(Expr)); e->kind=k; e->line=line; e->col=col; return e; }
Expr *parse_expr(TokStream *ts);

/* known struct tags, for disambiguating `Dog d;` local declarations */
int is_stag(const char *s){ for(size_t i=0;i<nstags;i++) if(!strcmp(stags[i],s)) return 1; return 0; }

static Expr *parse_primary(TokStream *ts){
    Token *t=peek(ts);
    if(t->kind==TK_FCONST){ advance(ts); Expr *e=E(E_FCONST,t->line,t->col); e->fval=t->fval; return e; }
    if(t->kind==TK_ICONST){ advance(ts); Expr *e=E(E_ICONST,t->line,t->col); e->ival=t->ival; return e; }
    if(t->kind==TK_KW_TRUE){ advance(ts); Expr *e=E(E_BOOL,t->line,t->col); e->bval=1; return e; }
    if(t->kind==TK_KW_FALSE){ advance(ts); Expr *e=E(E_BOOL,t->line,t->col); e->bval=0; return e; }
    if(t->kind==TK_KW_MAT){
        advance(ts); Expr *e=E(E_IDENT,t->line,t->col); char nm[16];
        snprintf(nm,sizeof nm,"mat%d",(int)t->ival);
        e->name=strdup(nm); return e; }
    if((t->kind==TK_KW_FLOAT||t->kind==TK_KW_INT||t->kind==TK_KW_UINT)&&t->ival>1){
        /* vector constructor: float4(...) etc. — synthesize the type name as the callee */
        advance(ts); Expr *e=E(E_IDENT,t->line,t->col); char nm[16];
        snprintf(nm,sizeof nm,"%s%d",t->kind==TK_KW_FLOAT?"float":t->kind==TK_KW_INT?"int":"uint",(int)t->ival);
        e->name=strdup(nm); return e; }
    if(t->kind==TK_KW_FLOAT||t->kind==TK_KW_HALF||t->kind==TK_KW_INT||t->kind==TK_KW_UINT||t->kind==TK_KW_BOOL){
        /* scalar constructor/cast: float(x) — synthesize the type name as the callee */
        advance(ts); Expr *e=E(E_IDENT,t->line,t->col);
        e->name=strdup(t->kind==TK_KW_FLOAT?"float":t->kind==TK_KW_HALF?"half":t->kind==TK_KW_INT?"int":t->kind==TK_KW_UINT?"uint":"bool");
        return e; }
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
        else if(ot->kind==TK_DOT){ advance(ts); Token *f=peek(ts);
            if(f->kind==TK_KW_SAMPLE){ /* Texture2DMS.sample[] property */ advance(ts);
                Expr *n=E(E_FIELD,ot->line,ot->col); n->operand=e; n->field=strdup("sample"); e=n; }
            else { expect(ts,TK_IDENT,"field after .");
                Expr *n=E(E_FIELD,ot->line,ot->col); n->operand=e; n->field=strdup(f->text); e=n; } }
        else if(ot->kind==TK_ARROW){ advance(ts); Token *f=peek(ts); expect(ts,TK_IDENT,"field after ->");
            Expr *d=E(E_DEREF,ot->line,ot->col); d->operand=e; /* p->f == (*p).f == p[id].f */
            Expr *n=E(E_FIELD,ot->line,ot->col); n->operand=d; n->field=strdup(f->text); e=n; }
        else if(ot->kind==TK_INC||ot->kind==TK_DEC){ advance(ts);
            Expr *n=E(E_INCDEC,ot->line,ot->col); n->operand=e; n->bval = ot->kind==TK_DEC; e=n; }
        else break;
    }
    return e;
}
/* does the token AFTER '(' begin a scalar/vector numeric type usable as a cast target? */
static int cast_type_start(TokStream *ts){
    TokKind k=(ts->i+1<ts->n)?ts->toks[ts->i+1].kind:TK_EOF;
    if(k==TK_IDENT&&g_tvar&&!strcmp(ts->toks[ts->i+1].text,g_tvar)) return 1; /* (T) in a template body */
    if(k==TK_IDENT&&is_stag(ts->toks[ts->i+1].text)) return 1; /* (S) struct cast */
    return k==TK_KW_FLOAT||k==TK_KW_HALF||k==TK_KW_INT||k==TK_KW_UINT||k==TK_KW_BOOL;
}
static Expr *parse_unary(TokStream *ts){
    Token *ut=peek(ts);
    if(ut->kind==TK_LPAREN&&cast_type_start(ts)){
        size_t save=ts->i;
        advance(ts); Type ty=parse_type(ts);
        /* only a cast when the type is immediately followed by ')': (float)x.
         * (float2(...)) or (T)(...) are parenthesized expressions. */
        if(peek(ts)->kind==TK_RPAREN){
            if(ty.is_ptr) die(ut->line,"pointer casts are not supported");
            if(ty.kind==T_VOID) die(ut->line,"cannot cast to void");
            advance(ts);
            Expr *o=parse_unary(ts); Expr *e=E(E_CAST,ut->line,ut->col); e->cty=ty; e->operand=o; return e;
        }
        ts->i=save; /* not a cast: fall through to the parenthesized expression */
    }
    if(ut->kind==TK_MINUS){ advance(ts); Expr *o=parse_unary(ts); Expr *e=E(E_NEG,ut->line,ut->col); e->operand=o; return e; }
    if(ut->kind==TK_PLUS){ advance(ts); return parse_unary(ts); } /* unary plus */
    if(ut->kind==TK_BANG){ advance(ts); Expr *o=parse_unary(ts); Expr *e=E(E_NOT,ut->line,ut->col); e->operand=o; return e; }
    if(ut->kind==TK_TILDE){ advance(ts); Expr *o=parse_unary(ts); Expr *e=E(E_COMPL,ut->line,ut->col); e->operand=o; return e; }
    if(ut->kind==TK_STAR){ advance(ts); Expr *o=parse_unary(ts); Expr *e=E(E_DEREF,ut->line,ut->col); e->operand=o; return e; }
    if(ut->kind==TK_INC||ut->kind==TK_DEC){ /* prefix ++x / --x */
        advance(ts); Expr *o=parse_unary(ts); Expr *e=E(E_INCDEC,ut->line,ut->col);
        e->operand=o; e->bval = ut->kind==TK_DEC; return e; }
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
Expr *parse_expr(TokStream *ts){ return parse_assign(ts); }

Stmt parse_stmt(TokStream *ts);
void blk_push(Block *b, Stmt s){
    b->stmts=realloc(b->stmts,(b->n+1)*sizeof(Stmt)); b->stmts[b->n++]=s;
}
Block parse_braced(TokStream *ts){ /* assumes '{' consumed */
    Stmt *s=NULL; size_t n=0,cap=0;
    while(peek(ts)->kind!=TK_RBRACE&&peek(ts)->kind!=TK_EOF){
        if(n==cap){cap=cap?cap*2:8;s=realloc(s,cap*sizeof(Stmt));} s[n++]=parse_stmt(ts); }
    expect(ts,TK_RBRACE,"}");
    return (Block){s,n};
}
Block parse_block_or_stmt(TokStream *ts){
    if(accept(ts,TK_LBRACE)) return parse_braced(ts);
    Stmt *s=malloc(sizeof(Stmt)); s[0]=parse_stmt(ts); return (Block){s,1};
}
Stmt parse_stmt(TokStream *ts){
    /* HLSL statement attributes: [unroll] / [branch] / [flatten] ... */
    while(peek(ts)->kind==TK_LBRACK && (ts->i+1<ts->n) && ts->toks[ts->i+1].kind==TK_IDENT){
        int depth=0;
        do{ Token *t=peek(ts);
            if(t->kind==TK_LBRACK)depth++;
            else if(t->kind==TK_RBRACK)depth--;
            if(t->kind==TK_EOF) die(t->line,"unterminated attribute");
            advance(ts); } while(depth>0);
    }
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
    if(kt->kind==TK_KW_SWITCH){
        advance(ts); expect(ts,TK_LPAREN,"("); Stmt st={0}; st.kind=S_SWITCH; st.line=kt->line; st.col=kt->col;
        st.sw_cond=parse_expr(ts); expect(ts,TK_RPAREN,")"); expect(ts,TK_LBRACE,"{");
        SCase *cs=NULL; size_t nc=0,cap=0;
        Block *cur=NULL;
        while(peek(ts)->kind!=TK_RBRACE&&peek(ts)->kind!=TK_EOF){
            if(peek(ts)->kind==TK_KW_CASE){
                advance(ts); /* 'case' */
                Expr *val=parse_expr(ts); expect(ts,TK_COLON,":");
                if(nc==cap){cap=cap?cap*2:4;cs=realloc(cs,cap*sizeof(SCase));}
                cs[nc].val=val; cs[nc].body=(Block){NULL,0}; cur=&cs[nc].body; nc++;
            } else if(peek(ts)->kind==TK_KW_DEFAULT){
                Token *dt=peek(ts); advance(ts); expect(ts,TK_COLON,":");
                if(st.has_default) die(dt->line,"duplicate default in switch");
                st.has_default=1; st.def_body=(Block){NULL,0}; cur=&st.def_body;
            } else {
                if(!cur) die(peek(ts)->line,"statement before the first case in switch");
                blk_push(cur,parse_stmt(ts));
            }
        }
        expect(ts,TK_RBRACE,"}");
        st.cases=cs; st.ncases=nc;
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
    if(kt->kind==TK_IDENT&&is_stag(kt->text)&&(ts->toks[ts->i+1].kind==TK_IDENT||ts->toks[ts->i+1].kind==TK_LT)){
        /* struct local: `Dog d;` / `Dog d = other;` / `Pair<float> p;` */
        Type ty=parse_type(ts);
        Token *nm=peek(ts); expect(ts,TK_IDENT,"name");
        Expr *init=NULL; if(accept(ts,TK_EQ)) init=parse_expr(ts);
        expect(ts,TK_SEMI,";");
        Stmt st={0}; st.kind=S_DECL; st.line=nm->line; st.col=nm->col; st.ty=ty; st.name=strdup(nm->text); st.init=init; return st;
    }
    /* HLSL struct methods: `uint getData() { ... }` inside a struct body */
    if(kt->kind==TK_IDENT && is_stag(kt->text) && (ts->i+1<ts->n) && ts->toks[ts->i+1].kind==TK_LPAREN){
        /* method declaration/definition on a struct local: skip the body */
        while(peek(ts)->kind!=TK_SEMI&&peek(ts)->kind!=TK_LBRACE&&peek(ts)->kind!=TK_EOF) advance(ts);
        if(peek(ts)->kind==TK_LBRACE){
            int depth=0;
            do{ Token *t=peek(ts);
                if(t->kind==TK_LBRACE)depth++;
                else if(t->kind==TK_RBRACE)depth--;
                if(t->kind==TK_EOF) die(t->line,"unterminated method body");
                advance(ts); } while(depth>0);
        } else if(peek(ts)->kind==TK_SEMI) advance(ts);
        Stmt st={0}; st.kind=S_EXPR; st.line=kt->line; st.col=kt->col; return st;
    }
    /* optional `const` qualifier on a local declaration */
    if(peek(ts)->kind==TK_KW_CONSTANT && ts->i+1<ts->n){
        TokKind nk=ts->toks[ts->i+1].kind;
        if(nk==TK_KW_FLOAT||nk==TK_KW_HALF||nk==TK_KW_INT||nk==TK_KW_UINT||nk==TK_KW_BOOL||nk==TK_KW_MAT)
            advance(ts); /* consume 'const', the type follows */
    }
    if(starts_scalar_type(ts)||(peek(ts)->kind==TK_IDENT&&g_tvar&&!strcmp(peek(ts)->text,g_tvar))){
        Type ty=parse_type(ts); Token *nm=peek(ts); expect(ts,TK_IDENT,"name");
        if(accept(ts,TK_LBRACK)){ Token *dn=peek(ts); expect(ts,TK_ICONST,"array extent"); ty.array_n=(int)dn->ival; expect(ts,TK_RBRACK,"]");
            if(accept(ts,TK_LBRACK)){ Token *dm=peek(ts); expect(ts,TK_ICONST,"array extent"); ty.array_m=(int)dm->ival; expect(ts,TK_RBRACK,"]"); } }
        Expr *init=NULL; if(accept(ts,TK_EQ)){
            if(peek(ts)->kind==TK_LBRACE){ /* HLSL brace initializer: `float4 x = {1,2,3,4};` — skip */
                int depth=0;
                do{ Token *t=peek(ts);
                    if(t->kind==TK_LBRACE)depth++;
                    else if(t->kind==TK_RBRACE)depth--;
                    if(t->kind==TK_EOF) die(t->line,"unterminated initializer");
                    advance(ts); } while(depth>0);
            } else init=parse_expr(ts);
        }
        if(peek(ts)->kind==TK_COLON){ /* HLSL local with : register(...) / : packoffset(...) */
            while(peek(ts)->kind!=TK_SEMI&&peek(ts)->kind!=TK_EOF) advance(ts);
        }
        expect(ts,TK_SEMI,";");
        Stmt st={0}; st.kind=S_DECL; st.line=nm->line; st.col=nm->col; st.ty=ty; st.name=strdup(nm->text); st.init=init;
        st.is_const = (kt->kind==TK_KW_CONSTANT); return st;
    }
    Expr *e=parse_expr(ts); expect(ts,TK_SEMI,";"); Stmt st={0}; st.kind=S_EXPR; st.line=e->line; st.col=e->col; st.expr=e; return st;
}

void parse_function(TokStream *ts, Program *prog){
    Stage stage=ST_NONE; if(accept(ts,TK_KW_VERTEX))stage=ST_VERTEX; else if(accept(ts,TK_KW_FRAGMENT))stage=ST_FRAGMENT;
    int is_kernel=accept(ts,TK_KW_KERNEL);
    /* template<typename T> — single type parameter, active for the signature and body */
    const char *tvar=NULL;
    if(accept(ts,TK_KW_TEMPLATE)){
        expect(ts,TK_LT,"<"); expect(ts,TK_KW_TYPENAME,"typename");
        Token *tn=peek(ts); expect(ts,TK_IDENT,"type parameter name");
        tvar=strdup(tn->text); expect(ts,TK_GT,">");
    }
    g_tvar=tvar;
    Type ret=parse_type(ts);
    if(stage!=ST_NONE&&is_kernel) die(peek(ts)->line,"render stages cannot also be kernels");
    if(is_kernel&&ret.kind!=T_VOID) die(peek(ts)->line,"kernel functions must return void");
    if(is_kernel&&tvar) die(peek(ts)->line,"kernels cannot be templates");
    if(ret.kind==T_STRUCT) { /* struct-by-value returns: plain + stage functions; kernels keep void */ }
    if(ret.matn) die(peek(ts)->line,"matrix-by-value return not supported");
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
        if(ty.kind==T_STRUCT&&!ty.is_ptr){
            if(is_kernel) die(pn->line,"kernel struct-by-value parameters not supported");
            if(stage!=ST_NONE&&stage!=ST_FRAGMENT) die(pn->line,"vertex struct parameters must be pointers");
        }
        if(ty.matn&&!ty.is_ptr) die(pn->line,"matrix-by-value parameter not supported");
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
    if(is_kernel&&ret.kind!=T_VOID) die(nm->line,"kernel functions must return void"); /* also covers coordN-implicit kernels */
    g_tvar=NULL; /* template type parameter ends with the function body */
    prog->funcs=realloc(prog->funcs,(prog->nfuncs+1)*sizeof(Function));
    prog->funcs[prog->nfuncs++]=(Function){strdup(nm->text),NULL,params,np,body,is_kernel,stage,ret,nm->line,tvar!=NULL,tvar?strdup(tvar):NULL,{0}};
}
void parse_struct(TokStream *ts, Program *prog){
    /* template<typename T> struct ... */
    const char *tvar=NULL;
    if(accept(ts,TK_KW_TEMPLATE)){
        expect(ts,TK_LT,"<"); expect(ts,TK_KW_TYPENAME,"typename");
        Token *tn=peek(ts); expect(ts,TK_IDENT,"type parameter name");
        tvar=strdup(tn->text); expect(ts,TK_GT,">");
    }
    g_tvar=tvar;
    expect(ts,TK_KW_STRUCT,"struct");
    Token *tag=peek(ts); expect(ts,TK_IDENT,"struct tag"); expect(ts,TK_LBRACE,"{");
    Field *f=NULL; size_t n=0,cap=0;
    while(peek(ts)->kind!=TK_RBRACE){
        /* HLSL struct method? `type name ( ... ) { ... }` — skip the body */
        if((ts->i+2<ts->n)&&ts->toks[ts->i+1].kind==TK_IDENT&&ts->toks[ts->i+2].kind==TK_LPAREN){
            advance(ts); advance(ts); /* type + method name */
            int mdepth=0;
            do{ Token *t=peek(ts);
                if(t->kind==TK_LPAREN)mdepth++;
                else if(t->kind==TK_RPAREN)mdepth--;
                if(t->kind==TK_EOF) die(t->line,"unterminated method signature");
                advance(ts); } while(mdepth>0);
            if(peek(ts)->kind==TK_LBRACE){
                int bdepth=0;
                do{ Token *t=peek(ts);
                    if(t->kind==TK_LBRACE)bdepth++;
                    else if(t->kind==TK_RBRACE)bdepth--;
                    if(t->kind==TK_EOF) die(t->line,"unterminated method body");
                    advance(ts); } while(bdepth>0);
            } else if(peek(ts)->kind==TK_SEMI) advance(ts);
            continue;
        }
        Type ty=parse_type(ts); if(ty.is_ptr) die(peek(ts)->line,"pointer fields unsupported");
        do{ Token *fn=peek(ts); expect(ts,TK_IDENT,"field name");
            /* optional HLSL-style semantic: `float4 pos : SV_POSITION;` */
            char *fsem=NULL;
            if(accept(ts,TK_COLON)){ Token *st=peek(ts); expect(ts,TK_IDENT,"semantic after :"); fsem=strdup(st->text); }
            /* optional [[...]] attribute: position / flat / color(N) / depth(any) / user(locnN) */
            int attr=0, attr_idx=0;
            if(accept(ts,TK_DBL_LBRACK)){
                Token *at=peek(ts); advance(ts);
                if(at->kind!=TK_IDENT) die(at->line,"expected an attribute name");
                if(!strcmp(at->text,"position")){ attr=1; }
                else if(!strcmp(at->text,"flat")){ attr=2; }
                else if(!strcmp(at->text,"depth")){ attr=4; expect(ts,TK_LPAREN,"("); Token *dq=peek(ts); advance(ts);
                    if(dq->kind!=TK_IDENT||strcmp(dq->text,"any")) die(dq->line,"[[depth(...)]] takes the value \"any\""); expect(ts,TK_RPAREN,")"); }
                else if(!strcmp(at->text,"color")||!strcmp(at->text,"user")){
                    attr = !strcmp(at->text,"color")?3:5; expect(ts,TK_LPAREN,"(");
                    Token *ix=peek(ts); if(ix->kind!=TK_ICONST) die(ix->line,"attribute index must be a constant");
                    attr_idx=(int)ix->ival; advance(ts); expect(ts,TK_RPAREN,")");
                }
                else die(at->line,"unknown field attribute [[%s]]",at->text);
                expect(ts,TK_DBL_RBRACK,"]]");
            }
            if(accept(ts,TK_LBRACK)){ Token *dn=peek(ts); expect(ts,TK_ICONST,"array extent"); ty.array_n=(int)dn->ival; expect(ts,TK_RBRACK,"]");
                if(accept(ts,TK_LBRACK)){ Token *dm=peek(ts); expect(ts,TK_ICONST,"array extent"); ty.array_m=(int)dm->ival; expect(ts,TK_RBRACK,"]"); } }
            if(n==cap){cap=cap?cap*2:8;f=realloc(f,cap*sizeof(Field));} f[n++]=(Field){strdup(fn->text),ty,attr,attr_idx,fsem};
        } while(accept(ts,TK_COMMA)); expect(ts,TK_SEMI,";");
    }
    expect(ts,TK_RBRACE,"}"); expect(ts,TK_SEMI,";");
    g_tvar=NULL; /* template type parameter ends with the struct body */
    prog->structs=realloc(prog->structs,(prog->nstructs+1)*sizeof(StructDef));
    prog->structs[prog->nstructs++]=(StructDef){strdup(tag->text),f,n,tvar!=NULL,tvar?strdup(tvar):NULL};
    stags=realloc(stags,(nstags+1)*sizeof(char*)); stags[nstags++]=prog->structs[prog->nstructs-1].tag;
}
/* tokens that may legally begin a statement or top-level construct; used by error recovery */
static int stmt_start(Token *t){
    switch(t->kind){
    case TK_KW_STRUCT: case TK_KW_RETURN: case TK_KW_BREAK: case TK_KW_CONTINUE:
    case TK_KW_IF: case TK_KW_WHILE: case TK_KW_FOR: case TK_KW_DO: case TK_KW_SWITCH:
    case TK_KW_CASE: case TK_KW_DEFAULT:
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
/* module-level constant: `constant <scalar-type> NAME = <literal>;` */
static void parse_const(TokStream *ts, Program *prog){
    Token *kt=peek(ts); advance(ts); /* 'constant' */
    Type ty=parse_type(ts);
    if(ty.is_ptr) die(kt->line,"constant globals cannot be pointers");
    if(ty.kind!=T_FLOAT&&ty.kind!=T_HALF&&ty.kind!=T_INT32&&ty.kind!=T_UINT32&&ty.kind!=T_BOOL)
        die(kt->line,"constant globals must be scalar numeric types");
    Token *nm=peek(ts); expect(ts,TK_IDENT,"constant name");
    expect(ts,TK_EQ,"=");
    Expr *init=parse_expr(ts); expect(ts,TK_SEMI,";");
    ConstDef cd={0}; cd.name=strdup(nm->text); cd.ty=ty; cd.line=nm->line;
    if(init->kind==E_ICONST){ cd.is_int=1; cd.ival=init->ival; }
    else if(init->kind==E_FCONST){ cd.is_int=0; cd.fval=init->fval; }
    else if(init->kind==E_BOOL){ cd.is_int=1; cd.ival=init->bval; cd.ty.kind=T_BOOL; }
    else die(nm->line,"constant initializer must be a literal");
    prog->consts=realloc(prog->consts,(prog->nconsts+1)*sizeof(ConstDef));
    prog->consts[prog->nconsts++]=cd;
}
Program parse_program(TokStream *ts){
    /* static so the struct and stream survive the longjmp in die() with defined values */
    static Program prog;
    static TokStream *cur;
    g_parse_prog=&prog;
    prog.structs=NULL; prog.nstructs=0; prog.funcs=NULL; prog.nfuncs=0;
    prog.consts=NULL; prog.nconsts=0;
    cur=ts;
    jmp_buf env;
    g_recover=&env;
    while(peek(cur)->kind!=TK_EOF){
        if(setjmp(env)==0){
            if(peek(cur)->kind==TK_KW_STRUCT){ parse_struct(cur,&prog); }
            else if(peek(cur)->kind==TK_KW_CONSTANT){ parse_const(cur,&prog); }
            else if(peek(cur)->kind==TK_KW_TEMPLATE){
                /* template<typename T> struct ... vs template<typename T> T f(...) */
                size_t save=cur->i;
                advance(cur); expect(cur,TK_LT,"<"); expect(cur,TK_KW_TYPENAME,"typename");
                advance(cur); expect(cur,TK_GT,">");
                int is_struct = peek(cur)->kind==TK_KW_STRUCT;
                cur->i=save;
                if(is_struct) parse_struct(cur,&prog);
                else parse_function(cur,&prog);
            }
            else parse_function(cur,&prog);
        } else {
            recover_skip(cur);
            /* swallow stray closers/semicolons left by the failed construct */
            while(peek(cur)->kind==TK_SEMI || peek(cur)->kind==TK_RBRACE) advance(cur);
        }
    }
    g_recover=NULL;
    return prog;
}
