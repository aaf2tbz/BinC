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
    TokKind rk=peek(ts)->kind;
    if(rk==TK_KW_STRUCTURED||rk==TK_KW_RWSTRUCTURED){
        advance(ts); Type et={0}; if(accept(ts,TK_LT)){ et=parse_type(ts); expect(ts,TK_GT,">"); } else et.kind=T_FLOAT;
        et.is_ptr=1; et.as=AS_DEVICE; et.array_n=0; et.array_m=0; return et;
    }
    if(rk==TK_KW_BYTEADDR||rk==TK_KW_RWBYTEADDR){
        advance(ts); Type et={0}; et.kind=T_UINT32; et.is_ptr=1; et.as=AS_DEVICE; et.struct_name=strdup("$byteaddr"); return et;
    }
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
    else if(accept(ts,TK_KW_BOOL)){t.kind=T_BOOL;t.vecn=(int)pt->ival;} else if(accept(ts,TK_KW_VOID))t.kind=T_VOID;
    else if(accept(ts,TK_KW_MAT)){ t.kind=T_FLOAT; t.matn=(int)pt->ival;
        /* non-square spellings (float2x3) parse into (matn=rows, matm=cols); */
        const char *mt=pt->text; char *x=mt?strchr(mt,'x'):NULL;
        if(x&&x[1]&&mt&&strncmp(mt,"float",5)==0){
            t.matm=atoi(x+1);
            if(t.matm==t.matn) t.matm=0;
        }
    }
    else if(peek(ts)->kind==TK_IDENT){
        /* HLSL template-style type spellings + template-arg struct fallback */
        const char *txt=peek(ts)->text;
        /* Resource spellings other than Texture2D are lexed as identifiers.
         * Struct fields use this shared parser, so preserve their opaque type
         * instead of misclassifying Texture3D/array/cube fields as structs. */
        int tex=0, dim=2, array=0, cube=0, rw=0;
        if(!strcmp(txt,"Texture1D")||!strcmp(txt,"Texture1DArray")||
           !strcmp(txt,"Texture2DArray")||!strcmp(txt,"Texture2DMS")||
           !strcmp(txt,"Texture2DMSArray")||!strcmp(txt,"Texture3D")||
           !strcmp(txt,"TextureCube")||!strcmp(txt,"TextureCubeArray")||
           !strcmp(txt,"RWTexture1D")||!strcmp(txt,"RWTexture1DArray")||
           !strcmp(txt,"RWTexture2DArray")||!strcmp(txt,"RWTexture3D")){
            tex=1; rw=!strncmp(txt,"RWTexture",9);
            dim=!strcmp(txt,"Texture1D")||!strcmp(txt,"Texture1DArray")||
                !strcmp(txt,"RWTexture1D")||!strcmp(txt,"RWTexture1DArray")?1:
                !strcmp(txt,"Texture3D")||!strcmp(txt,"RWTexture3D")?3:2;
            array=!strcmp(txt,"Texture1DArray")||!strcmp(txt,"Texture2DArray")||
                  !strcmp(txt,"Texture2DMSArray")||!strcmp(txt,"TextureCubeArray")||
                  !strcmp(txt,"RWTexture1DArray")||!strcmp(txt,"RWTexture2DArray");
            cube=!strcmp(txt,"TextureCube")||!strcmp(txt,"TextureCubeArray");
        }
        if(tex){
            advance(ts); t.kind=T_TEXTURE; t.tex_dim=dim; t.tex_array=array; t.tex_cube=cube; t.tex_rw=rw; t.tex_elt=T_FLOAT;
            if(accept(ts,TK_LT)){ Type et=parse_type(ts); t.tex_elt=et.kind; expect(ts,TK_GT,">"); }
            return t;
        }
        int has_lt = (ts->i+1<ts->n) && ts->toks[ts->i+1].kind==TK_LT;
        if(has_lt&&(!strcmp(txt,"matrix")||!strcmp(txt,"Matrix"))){
            advance(ts); expect(ts,TK_LT,"<");
            parse_type(ts); /* element type ignored */
            expect(ts,TK_COMMA,","); Token *rn=peek(ts); int rows=0,cols=0;
            if(rn->kind==TK_ICONST){ rows=(int)rn->ival; advance(ts); } else if(rn->kind==TK_IDENT){ advance(ts); } else die(rn->line,"expected matrix rows");
            expect(ts,TK_COMMA,","); Token *cn=peek(ts);
            if(cn->kind==TK_ICONST){ cols=(int)cn->ival; advance(ts); } else if(cn->kind==TK_IDENT){ advance(ts); } else die(cn->line,"expected matrix cols");
            expect(ts,TK_GT,">");
            t.kind=T_FLOAT; t.matn=rows; t.matm=rows&&cols&&rows==cols?0:cols;
            return t;
        }
        if(has_lt&&!strcmp(txt,"vector")){
            advance(ts); expect(ts,TK_LT,"<");
            Type et=parse_type(ts);
            expect(ts,TK_COMMA,","); Token *vn=peek(ts); int vsize=0;
            if(vn->kind==TK_ICONST){ vsize=(int)vn->ival; advance(ts); }
            else if(vn->kind==TK_IDENT){ advance(ts); }
            else die(vn->line,"expected vector size");
            expect(ts,TK_GT,">");
            et.vecn=vsize; if(et.vecn<2&&vsize>0)et.vecn=1; return et;
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
            /* generic templated type: consume a balanced <...> span. The
             * arguments may be symbolic expressions, not type/constant tokens. */
            advance(ts); expect(ts,TK_LT,"<"); int td=1;
            while(td>0){ Token *gt=peek(ts); if(gt->kind==TK_LT)td++; else if(gt->kind==TK_GT)td--; if(gt->kind==TK_EOF) die(gt->line,"unterminated template type"); advance(ts); }
            t.kind=T_STRUCT; t.struct_name=strdup(txt); t.is_ptr=0; return t;
        }
        if(!strcmp(txt,"min16float")||!strcmp(txt,"min10float")){ advance(ts); t.kind=T_FLOAT; return t; }
        if(!strcmp(txt,"matrix")||!strcmp(txt,"Matrix")){ advance(ts); t.kind=T_FLOAT; t.matn=4; return t; } /* bare matrix == float4x4 */
        if(!strcmp(txt,"vector")){ advance(ts); t.kind=T_FLOAT; t.vecn=4; return t; } /* bare vector == float4 */
        if(!strcmp(txt,"min16int")||!strcmp(txt,"min16uint")){ advance(ts); t.kind=!strcmp(txt,"min16int")?T_INT32:T_UINT32; return t; }
        if(!strcmp(txt,"int64_t")||!strcmp(txt,"int16_t")||!strcmp(txt,"int8_t")){ advance(ts); t.kind=T_INT32; return t; }
        if(!strcmp(txt,"uint64_t")||!strcmp(txt,"uint16_t")||!strcmp(txt,"uint8_t")){ advance(ts); t.kind=T_UINT32; return t; }
        if(!strcmp(txt,"dword")){ advance(ts); t.kind=T_UINT32; return t; } /* HLSL dword == uint32 */
        if(!strcmp(txt,"unsigned")||!strcmp(txt,"signed")){ /* `unsigned int x` / bare `unsigned` */
            advance(ts);
            if(peek(ts)->kind==TK_KW_INT||peek(ts)->kind==TK_KW_UINT){ t.kind=T_UINT32; advance(ts); return t; }
            t.kind=T_UINT32; return t; }
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
    else die(peek(ts)->line,"expected a type (got kind %d '%s')",peek(ts)->kind,peek(ts)->text);
    if(accept(ts,TK_STAR)){ t.is_ptr=1; if(t.as==0)t.as=AS_DEVICE; }
    return t;
}
/* fold a constant array-extent expression to a long (the defines have already
 * been substituted, so extents are pure arithmetic: MAX_POINTS/4 etc.) */
static long fold_ext(Expr *e){
    if(!e) return 0;
    switch(e->kind){
    case E_ICONST: return e->ival;
    case E_FCONST: return (long)e->fval;
    case E_BOOL:   return e->bval;
    case E_NEG:    return -fold_ext(e->operand);
    case E_BIN: { long l=fold_ext(e->lhs), r=fold_ext(e->rhs);
        switch(e->bop){
        case B_ADD: return l+r; case B_SUB: return l-r; case B_MUL: return l*r;
        case B_DIV: return r?l/r:0; case B_MOD: return r?l%r:0; default: return 0; } }
    default: return 0;
    }
}
/* `[expr]` array extent: a constant expression (MAX_POINTS/4, 3*4, ...) */
int parse_array_extent(TokStream *ts){
    if(peek(ts)->kind==TK_RBRACK) return 0; /* unsized `float a[] = {...}` */
    Expr *e=parse_expr(ts);
    return (int)fold_ext(e);
}
int starts_scalar_type(TokStream *ts){ TokKind k=peek(ts)->kind;    if(k==TK_IDENT){ /* HLSL template-style type spellings */
        const char *txt=peek(ts)->text;
        if((!strcmp(txt,"matrix")||!strcmp(txt,"Matrix")||!strcmp(txt,"vector")||!strcmp(txt,"ConstantBuffer")||!strcmp(txt,"TextureBuffer"))&&
           (ts->i+1<ts->n)&&ts->toks[ts->i+1].kind==TK_LT) return 1;
        if(!strcmp(txt,"matrix")||!strcmp(txt,"Matrix")||!strcmp(txt,"vector")) return 1; /* bare matrix/vector == float4x4/float4 */
        if(!strcmp(txt,"half")||(txt[0]=='h'&&txt[1]=='a'&&txt[2]=='l'&&txt[3]=='f'&&txt[4]>='1'&&txt[4]<='4'&&txt[5]=='\0')) return 1; /* half / half2/3/4 */
        if(!strcmp(txt,"min16float")||!strcmp(txt,"min10float")||!strcmp(txt,"min16int")||!strcmp(txt,"min16uint")) return 1;
        if(!strcmp(txt,"int64_t")||!strcmp(txt,"int16_t")||!strcmp(txt,"int8_t")) return 1;
        if(!strcmp(txt,"uint64_t")||!strcmp(txt,"uint16_t")||!strcmp(txt,"uint8_t")||
           !strcmp(txt,"dword")||!strcmp(txt,"word")||!strcmp(txt,"double")||
           !strcmp(txt,"half")||!strcmp(txt,"unsigned")||!strcmp(txt,"signed")) return 1;
    }
    return k==TK_KW_FLOAT||k==TK_KW_HALF||k==TK_KW_INT||k==TK_KW_UINT||k==TK_KW_BOOL||
           k==TK_KW_COORD||k==TK_KW_GRID_EXTENT||k==TK_KW_MAT||
           k==TK_KW_TEXTURE2D||k==TK_KW_RWTEXTURE2D||k==TK_KW_SAMPLER||
           k==TK_KW_STRUCTURED||k==TK_KW_RWSTRUCTURED||k==TK_KW_BYTEADDR||k==TK_KW_RWBYTEADDR; }

Expr *E(ExprKind k,int line,int col){ Expr *e=calloc(1,sizeof(Expr)); e->kind=k; e->line=line; e->col=col; return e; }
Expr *parse_expr(TokStream *ts);

static Expr *parse_initializer_list(TokStream *ts){
    Token *open=peek(ts); expect(ts,TK_LBRACE,"{");
    Expr *e=E(E_ARRAY,open->line,open->col);
    while(peek(ts)->kind!=TK_RBRACE&&peek(ts)->kind!=TK_EOF){
        Expr *item=(peek(ts)->kind==TK_LBRACE)?parse_initializer_list(ts):parse_expr(ts);
        e->args=realloc(e->args,(e->nargs+1)*sizeof *e->args);
        e->args[e->nargs++]=item;
        if(!accept(ts,TK_COMMA)) break;
    }
    expect(ts,TK_RBRACE,"}");
    return e;
}

static void skip_initializer_list(TokStream *ts){
    int depth=0;
    do{ Token *t=peek(ts);
        if(t->kind==TK_LBRACE) depth++;
        else if(t->kind==TK_RBRACE) depth--;
        if(t->kind==TK_EOF) die(t->line,"unterminated initializer");
        advance(ts);
    } while(depth>0);
}

/* known struct tags, for disambiguating `Dog d;` / `Pair<float> p;` locals */
int is_stag(const char *s){ for(size_t i=0;i<nstags;i++) if(!strcmp(stags[i],s)) return 1; return 0; }

static Expr *parse_primary(TokStream *ts){
    Token *t=peek(ts);
    if(t->kind==TK_FCONST){ advance(ts); Expr *e=E(E_FCONST,t->line,t->col); e->fval=t->fval; return e; }
    if(t->kind==TK_ICONST){ advance(ts); Expr *e=E(E_ICONST,t->line,t->col); e->ival=t->ival; return e; }
    if(t->kind==TK_KW_TRUE){ advance(ts); Expr *e=E(E_BOOL,t->line,t->col); e->bval=1; return e; }
    if(t->kind==TK_KW_FALSE){ advance(ts); Expr *e=E(E_BOOL,t->line,t->col); e->bval=0; return e; }
    if(t->kind==TK_KW_MAT){
        advance(ts); Expr *e=E(E_IDENT,t->line,t->col);
        if(t->text&&strstr(t->text,"x")) e->name=strdup(t->text);
        else { char nm[16]; snprintf(nm,sizeof nm,"mat%d",(int)t->ival); e->name=strdup(nm); }
        return e; }
    if((t->kind==TK_KW_FLOAT||t->kind==TK_KW_INT||t->kind==TK_KW_UINT||t->kind==TK_KW_BOOL)&&t->ival>1){
        /* vector constructor: float4(...) / bool3(...) etc. — synthesize the type name as the callee */
        advance(ts); Expr *e=E(E_IDENT,t->line,t->col); char nm[16];
        snprintf(nm,sizeof nm,"%s%d",t->kind==TK_KW_FLOAT?"float":t->kind==TK_KW_INT?"int":t->kind==TK_KW_UINT?"uint":"bool",(int)t->ival);
        e->name=strdup(nm); return e; }
    if(t->kind==TK_KW_FLOAT||t->kind==TK_KW_HALF||t->kind==TK_KW_INT||t->kind==TK_KW_UINT||t->kind==TK_KW_BOOL){
        /* scalar constructor/cast: float(x) — synthesize the type name as the callee */
        advance(ts); Expr *e=E(E_IDENT,t->line,t->col);
        e->name=strdup(t->kind==TK_KW_FLOAT?"float":t->kind==TK_KW_HALF?"half":t->kind==TK_KW_INT?"int":t->kind==TK_KW_UINT?"uint":"bool");
        return e; }
    if(t->kind==TK_IDENT||t->kind==TK_KW_IN||t->kind==TK_KW_OUT||t->kind==TK_KW_INOUT||
       t->kind==TK_KW_SAMPLE||t->kind==TK_KW_CENTROID||t->kind==TK_KW_LINEAR||t->kind==TK_KW_NOPERSPECTIVE||
       t->kind==TK_KW_STATIC||t->kind==TK_KW_REGISTER||t->kind==TK_KW_PACKOFFSET){
        /* HLSL code reuses in/out/inout/sample/... as variable names (`MRT out;`) */
        advance(ts); Expr *e=E(E_IDENT,t->line,t->col); e->name=strdup(t->text); return e; }
    if(accept(ts,TK_LPAREN)){ Expr *e=parse_expr(ts); expect(ts,TK_RPAREN,")"); return e; }
    die(t->line,"expected an expression (token kind %d '%s')",t->kind,t->text);
}
static Expr *parse_postfix(TokStream *ts){
    Expr *e=parse_primary(ts);
    for(;;){
        Token *ot=peek(ts);
        if(ot->kind==TK_LT && e->kind==E_IDENT){
            /* explicit template args: Name<T0, T1>(...) — the < opens the
             * template-arg list (not a comparison) when a call follows */
            size_t save=ts->i;
            advance(ts); int tdepth=1;
            while(tdepth>0&&peek(ts)->kind!=TK_EOF){
                Token *t2=peek(ts);
                if(t2->kind==TK_LT) tdepth++;
                else if(t2->kind==TK_GT) tdepth--;
                advance(ts);
            }
            if(peek(ts)->kind==TK_LPAREN){ continue; } /* the template call: re-loop onto the LPAREN branch */
            ts->i=save; /* not a template call — restore and let `<` be a comparison */
            ot=peek(ts);
        }
        if(ot->kind==TK_LPAREN){ /* calls may also target an atomic method (`acc->add`) */
            if(e->kind!=E_IDENT && !(e->kind==E_FIELD && e->field)) die(ot->line,"callee must be a function or method name");
            advance(ts); Expr *n=E(E_CALL,ot->line,ot->col); n->name=e->kind==E_IDENT?e->name:e->field; if(e->kind!=E_IDENT)n->callee=e;
            Expr **args=NULL; size_t na=0,cap=0;
            while(peek(ts)->kind!=TK_RPAREN){
                if(na==cap){cap=cap?cap*2:4;args=realloc(args,cap*sizeof(Expr*));}
                args[na++]=parse_expr(ts);
                if(!accept(ts,TK_COMMA))break; }
            expect(ts,TK_RPAREN,")");
            n->args=args; n->nargs=na; e=n;
            if(getenv("BINC_DEBUG_D3D9")&&n->name&&!strcmp(n->name,"IsNonZeroFast")) fprintf(stderr,"DBG callparse: na=%zu line=%d first_kind=%d\n",na,n->line,na?args[0]->kind:-1); }
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
        else if(ot->kind==TK_COLON&&ts->i+1<ts->n&&ts->toks[ts->i+1].kind==TK_COLON&&e->kind==E_IDENT){
            advance(ts); advance(ts); Token *q=peek(ts); expect(ts,TK_IDENT,"qualified constant name");
            char nm[256]; snprintf(nm,sizeof nm,"%s__%s",e->name,q->text); free(e->name); e->name=strdup(nm);
        }
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
    /* vector-type idents: half3/float4x3/min16float2 — the lexer emits the
     * whole spelling as one ident (parse_type accepts them) */
    if(k==TK_IDENT){
        const char *tx=ts->toks[ts->i+1].text;
        const char *bases[]={"min16float","min10float","min16int","min16uint","float","half","int","uint","bool","dword",NULL};
        for(int b=0;bases[b];b++){
            size_t bl=strlen(bases[b]);
            if(!strncmp(tx,bases[b],bl)&&tx[bl]>='1'&&tx[bl]<='4'&&(tx[bl+1]=='\0'||tx[bl+1]=='x')) return 1;
        }
    }
    /* unknown struct cast `(ViewState)0.0f` (struct from a dropped include):
     * treat as a cast only when the operand is a literal — never steal
     * parenthesized variables like `(bits) & ...`. An ident operand counts
     * when it ends the expression (statement/arg/close paren): `(S)Ident;` */
    if(k==TK_IDENT&&(ts->i+3<ts->n)&&ts->toks[ts->i+2].kind==TK_RPAREN){
        TokKind ok3=ts->toks[ts->i+3].kind;
        if(ok3==TK_FCONST||ok3==TK_ICONST) return 1;
        /* `(Unknown)-1` is a negative literal cast; `(value) - (other)`
         * is subtraction and must stay a parenthesized expression. */
        if(ok3==TK_MINUS&&(ts->i+4<ts->n)&&
           (ts->toks[ts->i+4].kind==TK_FCONST||ts->toks[ts->i+4].kind==TK_ICONST)) return 1;
        if(ok3==TK_IDENT){
            TokKind after=(ts->i+4<ts->n)?ts->toks[ts->i+4].kind:TK_EOF;
            if(after==TK_SEMI||after==TK_RPAREN||after==TK_COMMA) return 1;
        }
    }
    return k==TK_KW_FLOAT||k==TK_KW_HALF||k==TK_KW_INT||k==TK_KW_UINT||k==TK_KW_BOOL||k==TK_KW_MAT;
}
static Expr *parse_unary(TokStream *ts){
    Token *ut=peek(ts);
    if(getenv("BINC_DEBUG_D3D9")&&ut->kind==TK_LPAREN) fprintf(stderr,"DBG unary-paren line %d cts=%d k1=%d t1='%s' k2=%d t2='%s'\n",ut->line,cast_type_start(ts),(ts->i+1<ts->n)?ts->toks[ts->i+1].kind:-1,(ts->i+1<ts->n)?ts->toks[ts->i+1].text:"-",(ts->i+2<ts->n)?ts->toks[ts->i+2].kind:-1,(ts->i+2<ts->n)?ts->toks[ts->i+2].text:"-");
    if(ut->kind==TK_LPAREN&&cast_type_start(ts)){
        if(getenv("BINC_DEBUG_D3D9")) fprintf(stderr,"DBG cast? at line %d tok1='%s' tok2='%s'\n",ut->line,(ts->i+1<ts->n)?ts->toks[ts->i+1].text:"-",(ts->i+2<ts->n)?ts->toks[ts->i+2].text:"-");
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
        if(accept(ts,TK_LBRACK)){ /* array cast: `(float[4])x` */
            ty.array_n=parse_array_extent(ts); expect(ts,TK_RBRACK,"]");
            if(peek(ts)->kind==TK_RPAREN){
                advance(ts);
                Expr *o=parse_unary(ts); Expr *e=E(E_CAST,ut->line,ut->col); e->cty=ty; e->operand=o; return e;
            }
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
/* expect a name token: TK_IDENT, or (HLSL mode only — these kinds never
 * appear in .binc input) the in/out/inout qualifier keywords, which HLSL
 * code freely reuses as variable names (e.g. `PSInput out;`) */
void expect_name(TokStream *ts, const char *what){
    TokKind k=peek(ts)->kind;
    if(k==TK_IDENT||k==TK_KW_IN||k==TK_KW_OUT||k==TK_KW_INOUT||k==TK_KW_SAMPLE||k==TK_KW_CENTROID||
       k==TK_KW_LINEAR||k==TK_KW_NOPERSPECTIVE||k==TK_KW_STATIC||k==TK_KW_REGISTER||k==TK_KW_PACKOFFSET){
        advance(ts); return; }
    die(peek(ts)->line,"expected %s",what);
}
static int is_name_kind(TokKind k){ return k==TK_IDENT||k==TK_KW_IN||k==TK_KW_OUT||k==TK_KW_INOUT; }

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
    /* UE loop/branch attributes that stay bare idents when the SCW defines
     * are absent (the standalone Random.ush etc. don't include Platform.ush):
     * UNROLL/LOOP/BRANCH/FLATTEN/ALLOW_UAV_CONDITION before for/while/if;
     * UNROLL_N(N) too */
    while(peek(ts)->kind==TK_IDENT && (!strcmp(peek(ts)->text,"UNROLL")||!strcmp(peek(ts)->text,"UNROLL_N")||
                                       !strcmp(peek(ts)->text,"LOOP")||!strcmp(peek(ts)->text,"BRANCH")||
                                       !strcmp(peek(ts)->text,"FLATTEN")||!strcmp(peek(ts)->text,"ALLOW_UAV_CONDITION"))){
        advance(ts);
        if(peek(ts)->kind==TK_LPAREN){ int d=0;
            do{ Token *t=peek(ts);
                if(t->kind==TK_LPAREN)d++;
                else if(t->kind==TK_RPAREN)d--;
                if(t->kind==TK_EOF) die(t->line,"unterminated attribute args");
                advance(ts); } while(d>0); }
    }
    /* D3D9-era inline assembly: `asm { tfetch2D ... }` (Fxaa3_11) — skip */
    if(peek(ts)->kind==TK_IDENT&&!strcmp(peek(ts)->text,"asm")){
        Token *at=peek(ts); advance(ts);
        if(peek(ts)->kind==TK_LBRACE){
            int depth=0;
            do{ Token *t=peek(ts);
                if(t->kind==TK_LBRACE)depth++;
                else if(t->kind==TK_RBRACE)depth--;
                if(t->kind==TK_EOF) die(t->line,"unterminated asm block");
                advance(ts); } while(depth>0);
        }
        Stmt st={0}; st.kind=S_BLOCK; st.line=at->line; st.col=at->col; return st;
    }
    Token *kt=peek(ts);
    int local_const=0;
    if(kt->kind==TK_KW_STATIC){ advance(ts); kt=peek(ts); }
    if(kt->kind==TK_KW_CONSTANT && ts->i+1<ts->n){
        TokKind nk=ts->toks[ts->i+1].kind;
        if(nk==TK_KW_FLOAT||nk==TK_KW_HALF||nk==TK_KW_INT||nk==TK_KW_UINT||nk==TK_KW_BOOL||nk==TK_KW_MAT||nk==TK_IDENT||nk==TK_KW_STRUCT){
            advance(ts); local_const=1; kt=peek(ts);
        }
    }
    if(kt->kind==TK_SEMI){ advance(ts); Stmt st={0}; st.kind=S_BLOCK; st.line=kt->line; st.col=kt->col; return st; } /* empty statement `;` */
    if(kt->kind==TK_IDENT&&!strcmp(kt->text,"_Pragma")){ /* DXC _Pragma("dxc ...") inside bodies */
        while(peek(ts)->kind!=TK_SEMI&&peek(ts)->kind!=TK_RBRACE&&peek(ts)->kind!=TK_EOF) advance(ts);
        if(peek(ts)->kind==TK_SEMI) advance(ts);
        Stmt st={0}; st.kind=S_BLOCK; st.line=kt->line; st.col=kt->col; return st;
    }
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
            if(starts_scalar_type(ts)){ Type ty=parse_type(ts); Token *nm=peek(ts); expect_name(ts,"name");
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
    if(kt->kind==TK_IDENT&&(ts->i+1<ts->n)&&(is_name_kind(ts->toks[ts->i+1].kind)||ts->toks[ts->i+1].kind==TK_LT)){
        /* struct local: `Dog d;` / `Dog d = other;` / `Pair<float> p;` / `Dog d[3];` —
         * any ident type (`Type name;`), incl. structs from dropped #includes */
        Type ty=parse_type(ts);
        Token *nm=peek(ts); expect_name(ts,"name");
        int unsized_array=0;
        if(accept(ts,TK_LBRACK)){ unsized_array=(peek(ts)->kind==TK_RBRACK); ty.array_n=parse_array_extent(ts); expect(ts,TK_RBRACK,"]");
            if(accept(ts,TK_LBRACK)){ ty.array_m=parse_array_extent(ts); expect(ts,TK_RBRACK,"]"); } }
        Expr *init=NULL; if(accept(ts,TK_EQ)){
            if(peek(ts)->kind==TK_LBRACE){
                if(unsized_array||ty.array_n) init=parse_initializer_list(ts);
                else skip_initializer_list(ts);
            } else init=parse_expr(ts);
        }
        if(init&&init->kind==E_ARRAY&&unsized_array) ty.array_n=(int)init->nargs;
        if(accept(ts,TK_COMMA)){ /* struct comma-decl: `S x, y, z;` */
            Block b={0}; b.stmts=calloc(2,sizeof(Stmt));
            b.stmts[0]=(Stmt){.kind=S_DECL,.line=nm->line,.col=nm->col,.ty=ty,.name=strdup(nm->text),.init=init,.is_const=local_const}; b.n=1;
            do{
                Token *nm2=peek(ts); expect_name(ts,"name");
                Type ty2=ty;
                if(accept(ts,TK_LBRACK)){ ty2.array_n=parse_array_extent(ts); expect(ts,TK_RBRACK,"]"); }
                Expr *init2=NULL; if(accept(ts,TK_EQ)) init2=parse_expr(ts);
                b.stmts=realloc(b.stmts,(b.n+1)*sizeof(Stmt));
                b.stmts[b.n++]=(Stmt){.kind=S_DECL,.line=nm2->line,.col=nm2->col,.ty=ty2,.name=strdup(nm2->text),.init=init2};
            } while(accept(ts,TK_COMMA));
            expect(ts,TK_SEMI,";");
            Stmt bl={0}; bl.kind=S_BLOCK; bl.line=nm->line; bl.col=nm->col; bl.then_b=b; return bl;
        }
        expect(ts,TK_SEMI,";");
        Stmt st={0}; st.kind=S_DECL; st.line=nm->line; st.col=nm->col; st.ty=ty; st.name=strdup(nm->text); st.init=init; st.is_const=local_const; return st;
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
    /* Declaration qualifiers were consumed above so both scalar and struct
     * local declarations share the same `static`/`const` state. */
    if((peek(ts)->kind==TK_KW_FLOAT||peek(ts)->kind==TK_KW_HALF||peek(ts)->kind==TK_KW_INT||peek(ts)->kind==TK_KW_UINT||peek(ts)->kind==TK_KW_MAT)&&
       (ts->i+1<ts->n)&&ts->toks[ts->i+1].kind==TK_LPAREN){
        /* vector/matrix constructor expression statement: `float4(a,b) = rhs;` */
        Expr *e=parse_expr(ts); expect(ts,TK_SEMI,";");
        Stmt st={0}; st.kind=S_EXPR; st.line=e->line; st.col=e->col; st.expr=e; return st;
    }
    if(starts_scalar_type(ts)||(peek(ts)->kind==TK_IDENT&&((g_tvar&&!strcmp(peek(ts)->text,g_tvar))||is_stag(peek(ts)->text)
        ||((ts->i+1<ts->n)&&is_name_kind(ts->toks[ts->i+1].kind))))){
        Type ty=parse_type(ts); Token *nm=peek(ts); expect_name(ts,"name");
        int unsized_array=0;
        if(accept(ts,TK_LBRACK)){ unsized_array=(peek(ts)->kind==TK_RBRACK); ty.array_n=parse_array_extent(ts); expect(ts,TK_RBRACK,"]");
            if(accept(ts,TK_LBRACK)){ ty.array_m=parse_array_extent(ts); expect(ts,TK_RBRACK,"]"); } }
        Expr *init=NULL; if(accept(ts,TK_EQ)){
            if(peek(ts)->kind==TK_LBRACE){
                if(unsized_array||ty.array_n) init=parse_initializer_list(ts);
                else skip_initializer_list(ts);
            } else init=parse_expr(ts);
        }
        if(init&&init->kind==E_ARRAY&&unsized_array) ty.array_n=(int)init->nargs;
        if(peek(ts)->kind==TK_COLON){ /* HLSL local with : register(...) / : packoffset(...) */
            while(peek(ts)->kind!=TK_SEMI&&peek(ts)->kind!=TK_EOF) advance(ts);
        }
        Stmt st={0}; st.kind=S_DECL; st.line=nm->line; st.col=nm->col; st.ty=ty; st.name=strdup(nm->text); st.init=init;
        st.is_const = local_const;
        if(accept(ts,TK_COMMA)){ /* float3 x1, x2, x3; — a block of declarations */
            Block b={0}; b.stmts=calloc(2,sizeof(Stmt)); b.stmts[0]=st; b.n=1;
            do{
                Token *nm2=peek(ts); expect_name(ts,"name");
                Type ty2=ty; ty2.array_n=0; ty2.array_m=0;
                if(accept(ts,TK_LBRACK)){ ty2.array_n=parse_array_extent(ts); expect(ts,TK_RBRACK,"]");
                    if(accept(ts,TK_LBRACK)){ ty2.array_m=parse_array_extent(ts); expect(ts,TK_RBRACK,"]"); } }
                Expr *init2=NULL;
                if(accept(ts,TK_EQ)){ if(peek(ts)->kind==TK_LBRACE){ int depth=0;
                        do{ Token *t=peek(ts);
                            if(t->kind==TK_LBRACE)depth++;
                            else if(t->kind==TK_RBRACE)depth--;
                            if(t->kind==TK_EOF) die(t->line,"unterminated initializer");
                            advance(ts); } while(depth>0);
                    } else init2=parse_expr(ts); }
                b.stmts=realloc(b.stmts,(b.n+1)*sizeof(Stmt));
                b.stmts[b.n++]=(Stmt){.kind=S_DECL,.line=nm2->line,.col=nm2->col,.ty=ty2,.name=strdup(nm2->text),.init=init2,.is_const=st.is_const};
            } while(accept(ts,TK_COMMA));
            if(getenv("BINC_DEBUG_D3D9")) fprintf(stderr,"DBG declblock: next token kind=%d '%s' line=%d\n",peek(ts)->kind,peek(ts)->text,peek(ts)->line);
            expect(ts,TK_SEMI,";");
            Stmt bl={0}; bl.kind=S_BLOCK; bl.line=st.line; bl.col=st.col; bl.then_b=b; return bl;
        }
        expect(ts,TK_SEMI,";");
        return st;
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

    Token *nm=peek(ts); expect_name(ts,"function name"); expect(ts,TK_LPAREN,"(");
    Param *params=NULL; size_t np=0,cap=0;
    while(peek(ts)->kind!=TK_RPAREN){
        Uniformity un=UN_UNIFORM; if(accept(ts,TK_KW_VARYING))un=UN_VARYING; else accept(ts,TK_KW_UNIFORM);
        Type ty=parse_type(ts); Token *pn=peek(ts); expect_name(ts,"param name");
        if(accept(ts,TK_LBRACK)){
            ty.array_n=parse_array_extent(ts); expect(ts,TK_RBRACK,"]");
            if(accept(ts,TK_LBRACK)){ ty.array_m=parse_array_extent(ts); expect(ts,TK_RBRACK,"]"); }
            if(ty.as!=AS_THREADGROUP) die(pn->line,"only threadgroup parameters may be arrays");
        }
        if(ty.kind==T_STRUCT&&!ty.is_ptr){
            if(is_kernel) die(pn->line,"kernel struct-by-value parameters not supported");
            if(stage!=ST_NONE&&stage!=ST_FRAGMENT) die(pn->line,"vertex struct parameters must be pointers");
        }

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
static int nested_enum_eval(Expr *e, long *out){
    if(!e) return 0;
    if(e->kind==E_ICONST){ *out=(long)e->ival; return 1; }
    if(e->kind==E_BOOL){ *out=e->bval?1:0; return 1; }
    if(e->kind==E_IDENT&&g_parse_prog){
        for(size_t i=0;i<g_parse_prog->nconsts;i++){
            const char *nm=g_parse_prog->consts[i].name; const char *p=strstr(nm,"__"); p=p?p+2:nm;
            if(!strcmp(nm,e->name)||!strcmp(p,e->name)){ *out=g_parse_prog->consts[i].ival; return 1; }
        }
        return 0;
    }
    if(e->kind==E_NEG||e->kind==E_COMPL){ long v; if(!nested_enum_eval(e->operand,&v)) return 0; *out=e->kind==E_NEG?-v:~v; return 1; }
    if(e->kind==E_BIN){ long a,b; if(!nested_enum_eval(e->lhs,&a)||!nested_enum_eval(e->rhs,&b)) return 0; switch(e->bop){ case B_ADD:*out=a+b;return 1; case B_SUB:*out=a-b;return 1; case B_MUL:*out=a*b;return 1; case B_DIV:if(!b)return 0;*out=a/b;return 1; case B_MOD:if(!b)return 0;*out=a%b;return 1; case B_AND:*out=a&b;return 1; case B_OR:*out=a|b;return 1; case B_XOR:*out=a^b;return 1; case B_SHL:*out=a<<b;return 1; case B_SHR:*out=a>>b;return 1; default:return 0; } }
    return 0;
}

static void parse_nested_enum(TokStream *ts, const char *scope){
    advance(ts); if(peek(ts)->kind==TK_IDENT) advance(ts); if(accept(ts,TK_COLON)){ if(peek(ts)->kind==TK_KW_UINT||peek(ts)->kind==TK_KW_INT) advance(ts); }
    expect(ts,TK_LBRACE,"{"); long prev=0; int have=0;
    while(peek(ts)->kind!=TK_RBRACE&&peek(ts)->kind!=TK_EOF){
        Token *nt=peek(ts); expect(ts,TK_IDENT,"enum name"); long value=have?prev+1:0;
        if(accept(ts,TK_EQ)){ Expr *init=parse_expr(ts); if(!nested_enum_eval(init,&value)) die(nt->line,"nested enum initializer must be integral"); }
        if(g_parse_prog){ ConstDef c={0}; char nm[256]; snprintf(nm,sizeof nm,"%s__%s",scope,nt->text); c.name=strdup(nm); c.ty.kind=T_INT32; c.is_int=1; c.ival=value; c.fval=(double)value; c.line=nt->line; c.mut=0; g_parse_prog->consts=realloc(g_parse_prog->consts,(g_parse_prog->nconsts+1)*sizeof c); g_parse_prog->consts[g_parse_prog->nconsts++]=c; }
        prev=value; have=1; if(!accept(ts,TK_COMMA)) break;
    }
    expect(ts,TK_RBRACE,"}"); if(peek(ts)->kind==TK_SEMI) advance(ts);
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
    Token *tag=peek(ts); expect_name(ts,"struct tag"); expect(ts,TK_LBRACE,"{");
    Field *f=NULL; size_t n=0,cap=0;
    while(peek(ts)->kind!=TK_RBRACE){
        if(peek(ts)->kind==TK_KW_ENUM){ parse_nested_enum(ts,tag->text); continue; }
        if(peek(ts)->kind==TK_KW_TEMPLATE){
            advance(ts); if(accept(ts,TK_LT)){ int td=1; while(td>0){ Token *tt=peek(ts); if(tt->kind==TK_LT)td++; else if(tt->kind==TK_GT)td--; if(tt->kind==TK_EOF) die(tt->line,"unterminated method template"); advance(ts); } }
            continue;
        }
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
        int skip_static_member=0;
        while((peek(ts)->kind==TK_IDENT&&(!strcmp(peek(ts)->text,"row_major")||!strcmp(peek(ts)->text,"column_major")||
                                         !strcmp(peek(ts)->text,"precise")||!strcmp(peek(ts)->text,"shared")||!strcmp(peek(ts)->text,"nointerpolation")))||
              peek(ts)->kind==TK_KW_LINEAR||peek(ts)->kind==TK_KW_NOPERSPECTIVE||
              peek(ts)->kind==TK_KW_CENTROID||peek(ts)->kind==TK_KW_SAMPLE||peek(ts)->kind==TK_KW_STATIC||peek(ts)->kind==TK_KW_CONSTANT){
            if(peek(ts)->kind==TK_KW_STATIC||peek(ts)->kind==TK_KW_CONSTANT) skip_static_member=1;
            advance(ts);
        }
        if(peek(ts)->kind==TK_IDENT&&ts->i+1<ts->n&&ts->toks[ts->i+1].kind==TK_LT){
            size_t j=ts->i+1; int d=0; for(;j<ts->n;j++){ if(ts->toks[j].kind==TK_LT)d++; else if(ts->toks[j].kind==TK_GT&&!--d) break; }
            if(j+2<ts->n&&((ts->toks[j+1].kind==TK_IDENT)||(ts->toks[j+1].kind==TK_KW_CONSTANT))&&(ts->toks[j+2].kind==TK_LPAREN||(ts->toks[j+1].kind==TK_IDENT&&ts->toks[j+1].text&&!strcmp(ts->toks[j+1].text,"operator")&&ts->toks[j+2].kind==TK_LBRACK))){
                while(ts->i<=j+1) advance(ts); if(accept(ts,TK_LBRACK)) expect(ts,TK_RBRACK,"]"); int md=0; do{ Token *t=peek(ts); if(t->kind==TK_LPAREN)md++; else if(t->kind==TK_RPAREN)md--; if(t->kind==TK_EOF) die(t->line,"unterminated method signature"); advance(ts); }while(md>0);
                if(peek(ts)->kind==TK_LBRACE){ int bd=0; do{ Token *t=peek(ts); if(t->kind==TK_LBRACE)bd++; else if(t->kind==TK_RBRACE)bd--; if(t->kind==TK_EOF) die(t->line,"unterminated method body"); advance(ts); }while(bd>0); }
                continue;
            }
        }
        if(peek(ts)->kind==TK_KW_CONSTANT&&ts->i+1<ts->n&&ts->toks[ts->i+1].kind==TK_LPAREN){
            advance(ts); int md=0; do{ Token *t=peek(ts); if(t->kind==TK_LPAREN)md++; else if(t->kind==TK_RPAREN)md--; if(t->kind==TK_EOF) die(t->line,"unterminated method signature"); advance(ts); }while(md>0);
            if(peek(ts)->kind==TK_LBRACE){ int bd=0; do{ Token *t=peek(ts); if(t->kind==TK_LBRACE)bd++; else if(t->kind==TK_RBRACE)bd--; if(t->kind==TK_EOF) die(t->line,"unterminated method body"); advance(ts); }while(bd>0); }
            continue;
        }
        Type ty=parse_type(ts);
        int was_method=0;
        do{ Token *fn=peek(ts); expect_name(ts,"field name");
            if(skip_static_member&&accept(ts,TK_EQ)){ int d=0; while(peek(ts)->kind!=TK_EOF){ Token *it=peek(ts); if(it->kind==TK_LPAREN||it->kind==TK_LBRACK||it->kind==TK_LBRACE)d++; else if(it->kind==TK_RPAREN||it->kind==TK_RBRACK||it->kind==TK_RBRACE){ if(d>0)d--; } if(it->kind==TK_SEMI&&d==0){ advance(ts); break; } advance(ts); } was_method=1; break; }
            int is_oper = fn && !strcmp(fn->text,"operator") && (ts->i+1<ts->n) && ts->toks[ts->i+1].kind!=TK_IDENT;
            if(peek(ts)->kind==TK_LPAREN || is_oper){
                /* struct method with a complex (template-instantiation) return
                 * type — the (i+1)==IDENT heuristic misses `TDual<T> f(...)`;
                 * `operator+`/`operator-` lex as IDENT PLUS/MINUS (skip the
                 * operator token, then the signature follows) */
                if(is_oper){ advance(ts); if(accept(ts,TK_LBRACK)) expect(ts,TK_RBRACK,"]"); }
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
                was_method=1;
                break;
            }
            /* optional HLSL array extent BEFORE the semantic: `uint4 Verts[3] : CONTROLPOINTS;` */
            if(accept(ts,TK_LBRACK)){ ty.array_n=parse_array_extent(ts); expect(ts,TK_RBRACK,"]");
                if(accept(ts,TK_LBRACK)){ ty.array_m=parse_array_extent(ts); expect(ts,TK_RBRACK,"]"); } }
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
                if(accept(ts,TK_DBL_RBRACK)){} else { expect(ts,TK_RBRACK,"]"); expect(ts,TK_RBRACK,"]"); } /* `]]` (lexer may emit one or two tokens) */
            }
            if(accept(ts,TK_LBRACK)){ ty.array_n=parse_array_extent(ts); expect(ts,TK_RBRACK,"]");
                if(accept(ts,TK_LBRACK)){ ty.array_m=parse_array_extent(ts); expect(ts,TK_RBRACK,"]"); } }
            if(n==cap){cap=cap?cap*2:8;f=realloc(f,cap*sizeof(Field));} f[n++]=(Field){strdup(fn->text),ty,attr,attr_idx,fsem};
        } while(accept(ts,TK_COMMA));
        if(!was_method) expect(ts,TK_SEMI,";");
        was_method=0;
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
