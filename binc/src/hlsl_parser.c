/* hlsl_parser.c — HLSL top-level parser (sm5 surface, Phases 1-7 of the
 * HLSL-to-Metal plan). Produces an HLSLProg (functions, structs, cbuffers,
 * module globals) which hlsl_build() (hlsl_lower.c) lowers onto the shared
 * BinC Program AST. Statement and expression bodies reuse the shared parser
 * (parser.c); the lexer is the shared tokenizer extended with HLSL keywords.
 *
 * Parse-only surface here: structs (with `: semantics`), cbuffer/tbuffer
 * blocks, groupshared/static/const globals, functions with [numthreads],
 * in/out/inout params, `: semantics` on params and returns, and the HLSL
 * resource types (StructuredBuffer<>, RWTexture2D<>, ...). */
#include "binc.h"
#include <stdlib.h>
#include <string.h>

/* HLSL resource/typed-buffer spellings; everything else uses parse_type */
static Type hlsl_type(TokStream *ts){
    TokKind k=peek(ts)->kind;
    /* HLSL variable modifiers: row_major / column_major / precise / shared */
    while(k==TK_IDENT&&((!strcmp(peek(ts)->text,"row_major"))||(!strcmp(peek(ts)->text,"column_major"))||
                        (!strcmp(peek(ts)->text,"precise"))||(!strcmp(peek(ts)->text,"shared")))){
        advance(ts); k=peek(ts)->kind;
    }
    if(k==TK_KW_RWTEXTURE2D){
        advance(ts); Type t={0}; t.kind=T_TEXTURE;
        if(accept(ts,TK_LT)){ Type et=parse_type(ts);
            t.tex_elt=et.kind;
            if(t.tex_elt!=T_FLOAT&&t.tex_elt!=T_HALF&&t.tex_elt!=T_INT32&&t.tex_elt!=T_UINT32)
                die(peek(ts)->line,"unsupported texture element type");
            expect(ts,TK_GT,">"); }
        else t.tex_elt=T_FLOAT; /* bare RWTexture2D defaults to float */
        return t;
    }
    if(k==TK_KW_TEXTURE2D||k==TK_IDENT){
        /* Texture2D / Texture1D / Texture3D / TextureCube / Texture2DArray /
         * TextureCubeArray / RWTexture2DArray / RWTexture3D ... -> T_TEXTURE */
        const char *txt=k==TK_KW_TEXTURE2D?"Texture2D":peek(ts)->text;
        if(k==TK_KW_TEXTURE2D||!strcmp(txt,"Texture1D")||!strcmp(txt,"Texture3D")||
           !strcmp(txt,"TextureCube")||!strcmp(txt,"Texture2DArray")||!strcmp(txt,"TextureCubeArray")||
           !strcmp(txt,"RWTexture2DArray")||!strcmp(txt,"RWTexture3D")||!strcmp(txt,"RWTexture1D")||
           !strcmp(txt,"Texture1DArray")||!strcmp(txt,"RWTexture1DArray")||
           !strcmp(txt,"Texture2DMS")||!strcmp(txt,"Texture2DMSArray")){
            advance(ts); Type t={0}; t.kind=T_TEXTURE;
            if(accept(ts,TK_LT)){ Type et=parse_type(ts); t.tex_elt=et.kind;
                if(accept(ts,TK_COMMA)){ /* Texture2DMS<T, S>: sample count */
                    Token *sc=peek(ts); expect(ts,TK_ICONST,"MS sample count"); (void)sc;
                }
                expect(ts,TK_GT,">"); }
            else t.tex_elt=T_FLOAT;
            return t;
        }
    }
    if(k==TK_KW_STRUCTURED||k==TK_KW_RWSTRUCTURED){
        /* StructuredBuffer<T> / RWStructuredBuffer<T> -> device pointer to T */
        advance(ts); Type et={0};
        if(accept(ts,TK_LT)){ et=parse_type(ts); expect(ts,TK_GT,">"); }
        else et.kind=T_FLOAT; /* bare Buffer defaults to float */
        et.is_ptr=1; et.as=AS_DEVICE; et.array_n=0; et.array_m=0;
        return et;
    }
    if(k==TK_KW_BYTEADDR||k==TK_KW_RWBYTEADDR){
        /* ByteAddressBuffer -> raw uint device buffer */
        advance(ts); Type t={0}; t.kind=T_UINT32; t.is_ptr=1; t.as=AS_DEVICE; return t;
    }
    return parse_type(ts);
}

/* [numthreads(8,8,1)] and other [attributes]: extract numthreads, skip the rest */
static void parse_attributes(TokStream *ts, HLSLFunc *f){
    while(peek(ts)->kind==TK_LBRACK){
        advance(ts); /* '[' */
        Token *a=peek(ts);
        if(a->kind==TK_IDENT&&!strcmp(a->text,"numthreads")){
            advance(ts); expect(ts,TK_LPAREN,"(");
            Token *tx=peek(ts); expect(ts,TK_ICONST,"numthreads x"); f->numtx=(int)tx->ival; expect(ts,TK_COMMA,",");
            Token *ty=peek(ts); expect(ts,TK_ICONST,"numthreads y"); f->numty=(int)ty->ival; expect(ts,TK_COMMA,",");
            Token *tz=peek(ts); expect(ts,TK_ICONST,"numthreads z"); f->numtz=(int)tz->ival;
            expect(ts,TK_RPAREN,")"); f->has_numthreads=1;
        } else {
            int depth=1;
            while(depth>0){
                Token *t=peek(ts);
                if(t->kind==TK_LBRACK)depth++;
                else if(t->kind==TK_RBRACK)depth--;
                if(t->kind==TK_EOF) die(t->line,"unterminated attribute");
                advance(ts);
            }
            continue; /* attribute consumed, loop for more */
        }
        expect(ts,TK_RBRACK,"]");
    }
}

/* optional `: register(bN)` / `: packoffset(cN.x)` suffix — parsed, ignored for now.
 * HLSL allows several in a row: `float4 f : register(vs, c0) : register(ps, c1);` */
static void skip_register_suffix(TokStream *ts){
    for(;;){
        if(!accept(ts,TK_COLON)) return;
        if(accept(ts,TK_KW_REGISTER)){ expect(ts,TK_LPAREN,"(");
            while(peek(ts)->kind!=TK_RPAREN&&peek(ts)->kind!=TK_EOF) advance(ts);
            expect(ts,TK_RPAREN,")"); continue; }
        if(accept(ts,TK_KW_PACKOFFSET)){ expect(ts,TK_LPAREN,"(");
            while(peek(ts)->kind!=TK_RPAREN&&peek(ts)->kind!=TK_EOF) advance(ts);
            expect(ts,TK_RPAREN,")"); continue; }
        /* bare semantic-ish ident: consume it */
        if(peek(ts)->kind!=TK_SEMI&&peek(ts)->kind!=TK_EOF) advance(ts);
        return;
    }
}

static void hpush(void **arr, size_t *n, size_t *cap, void *item, size_t sz){
    if(*n==*cap){*cap=*cap?*cap*2:8; *arr=realloc(*arr,*cap*sz);} 
    memcpy((char*)*arr+(*n)*sz,item,sz); (*n)++;
}

HLSLProg hlsl_parse(TokStream *ts){
    HLSLProg hp={0}; size_t fcap=0,scap=0,ccap=0,gcap=0;
    Program tmp={0};
    while(peek(ts)->kind!=TK_EOF){
        /* leading attributes before a function */
        HLSLFunc attrs={0};
        parse_attributes(ts,&attrs);
        Token *kt=peek(ts);

        if(kt->kind==TK_KW_STRUCT){
            /* anonymous `struct { ... }` (with a `{...}` initializer) — parse
             * the shared way only for named structs; anonymous ones are
             * skipped to the closing brace + ';' */
            if(ts->i+1<ts->n && ts->toks[ts->i+1].kind==TK_LBRACE){
                /* anonymous `struct { ... }` — skip to the closing brace + ';' */
                advance(ts); /* struct */
                expect(ts,TK_LBRACE,"{"); int depth=1;
                while(depth>0){
                    Token *t=peek(ts);
                    if(t->kind==TK_LBRACE)depth++;
                    else if(t->kind==TK_RBRACE)depth--;
                    if(t->kind==TK_EOF) die(t->line,"unterminated struct");
                    advance(ts);
                }
                if(peek(ts)->kind==TK_IDENT) advance(ts); /* instance name */
                if(peek(ts)->kind==TK_EQ){ /* {..} initializer */
                    advance(ts);
                    if(peek(ts)->kind==TK_LBRACE){
                        int d2=0;
                        do{ Token *t=peek(ts);
                            if(t->kind==TK_LBRACE)d2++;
                            else if(t->kind==TK_RBRACE)d2--;
                            if(t->kind==TK_EOF) die(t->line,"unterminated initializer");
                            advance(ts); } while(d2>0);
                    } else while(peek(ts)->kind!=TK_SEMI&&peek(ts)->kind!=TK_EOF) advance(ts);
                }
                expect(ts,TK_SEMI,";");
                continue;
            }
            g_parse_prog=&tmp;
            parse_struct(ts,&tmp);
            g_parse_prog=NULL;
            if(tmp.nstructs){
                HLSLStruct hs={0};
                hs.tag=strdup(tmp.structs[0].tag);
                hs.fields=tmp.structs[0].fields; hs.nfields=tmp.structs[0].nfields;
                hpush((void**)&hp.structs,&hp.nstructs,&scap,&hs,sizeof hs);
                tmp.structs=NULL; tmp.nstructs=0;
            }
            continue;
        }
        if(kt->kind==TK_KW_CBUFFER||kt->kind==TK_KW_TBUFFER){
            advance(ts); /* 'cbuffer' / 'tbuffer' */
            HLCBuf cb={0};
            Token *bn=peek(ts); expect(ts,TK_IDENT,"cbuffer name"); cb.name=strdup(bn->text);
            skip_register_suffix(ts); /* cbuffer X : register(b0) { */
            expect(ts,TK_LBRACE,"{");
            Field *f=NULL; size_t n=0,cap=0;
            while(peek(ts)->kind!=TK_RBRACE&&peek(ts)->kind!=TK_EOF){
                Type ty=hlsl_type(ts);
                do{
                    Token *fn=peek(ts); expect(ts,TK_IDENT,"cbuffer field name");
                    char *sem=NULL;
                    if(accept(ts,TK_COLON)){ Token *st=peek(ts); expect(ts,TK_IDENT,"semantic"); sem=strdup(st->text); }
                    skip_register_suffix(ts); /* : register(c0) / : packoffset(c0.x) */
                    if(n==cap){cap=cap?cap*2:8;f=realloc(f,cap*sizeof(Field));}
                    f[n++]=(Field){strdup(fn->text),ty,0,0,sem};
                } while(accept(ts,TK_COMMA));
                expect(ts,TK_SEMI,";");
            }
            expect(ts,TK_RBRACE,"}");
            skip_register_suffix(ts);
            expect(ts,TK_SEMI,";");
            cb.fields=f; cb.nfields=n;
            hpush((void**)&hp.cbufs,&hp.ncbufs,&ccap,&cb,sizeof cb);
            continue;
        }
        if(kt->kind==TK_KW_GROUPSHARED||kt->kind==TK_KW_STATIC||kt->kind==TK_KW_CONSTANT){
            int gs=accept(ts,TK_KW_GROUPSHARED);
            int st=accept(ts,TK_KW_STATIC);
            int cn=accept(ts,TK_KW_CONSTANT);
            if(peek(ts)->kind==TK_KW_STRUCT&&(ts->i+1<ts->n)&&ts->toks[ts->i+1].kind==TK_LBRACE){
                /* static const struct { ... } x = {...}; — anonymous: skip to ';' */
                advance(ts); /* struct */
                expect(ts,TK_LBRACE,"{"); int depth=1;
                while(depth>0){
                    Token *t=peek(ts);
                    if(t->kind==TK_LBRACE)depth++;
                    else if(t->kind==TK_RBRACE)depth--;
                    if(t->kind==TK_EOF) die(t->line,"unterminated struct");
                    advance(ts);
                }
                if(peek(ts)->kind==TK_IDENT) advance(ts); /* struct instance name */
                if(peek(ts)->kind==TK_EQ){
                    advance(ts);
                    if(peek(ts)->kind==TK_LBRACE){
                        int d2=0;
                        do{ Token *t=peek(ts);
                            if(t->kind==TK_LBRACE)d2++;
                            else if(t->kind==TK_RBRACE)d2--;
                            if(t->kind==TK_EOF) die(t->line,"unterminated initializer");
                            advance(ts); } while(d2>0);
                    } else while(peek(ts)->kind!=TK_SEMI&&peek(ts)->kind!=TK_EOF) advance(ts);
                }
                expect(ts,TK_SEMI,";");
                continue;
            }
            Type ty=hlsl_type(ts);
            HLSLGlobal gg={0};
            Token *gn=peek(ts); expect(ts,TK_IDENT,"global name");
            gg.name=strdup(gn->text); gg.ty=ty; gg.is_groupshared=gs; gg.is_const=cn||st; gg.line=gn->line;
            if(accept(ts,TK_EQ)){
                gg.has_init=1;
                Expr *e=parse_expr(ts);
                if(e->kind==E_ICONST){ gg.is_int=1; gg.ival=e->ival; }
                else if(e->kind==E_FCONST){ gg.fval=e->fval; }
                else if(e->kind==E_BOOL){ gg.is_int=1; gg.ival=e->bval; }
                /* non-literal initializers (float4(...), other calls) are
                 * parsed and discarded — the lowering only materializes
                 * literal consts (Phase 4 wires real constant buffers) */
            }
            if(accept(ts,TK_LBRACK)){ /* global array [N] or [N][M] */
                Token *dn=peek(ts); expect(ts,TK_ICONST,"array extent"); gg.ty.array_n=(int)dn->ival; expect(ts,TK_RBRACK,"]");
                if(accept(ts,TK_LBRACK)){ Token *dm=peek(ts); expect(ts,TK_ICONST,"array extent"); gg.ty.array_m=(int)dm->ival; expect(ts,TK_RBRACK,"]"); }
            }
            skip_register_suffix(ts);
            expect(ts,TK_SEMI,";");
            hpush((void**)&hp.globals,&hp.nglobals,&gcap,&gg,sizeof gg);
            continue;
        }
        /* function definition, or an unqualified module global
         * (StructuredBuffer<>, SamplerState, plain typed vars, ...) */
        size_t save = ts->i;
        HLSLFunc hf=attrs; /* attributes (numthreads) carried over */
        hf.is_export=accept(ts,TK_KW_STATIC);
        accept(ts,TK_KW_IN); /* legal `in` before the return type */
        hf.ret=hlsl_type(ts);
        Token *fn=peek(ts);
        int is_fn = fn->kind==TK_IDENT && (ts->i+1<ts->n) && ts->toks[ts->i+1].kind==TK_LPAREN;
        if(!is_fn){
            ts->i=save;
            Type gty=hlsl_type(ts);
            HLSLGlobal gg={0};
            Token *gn=peek(ts); expect(ts,TK_IDENT,"global name");
            gg.name=strdup(gn->text); gg.ty=gty; gg.line=gn->line;
            if(accept(ts,TK_LBRACK)){ /* global array [N] or [N][M] */
                Token *dn=peek(ts); expect(ts,TK_ICONST,"array extent"); gty.array_n=(int)dn->ival; expect(ts,TK_RBRACK,"]");
                if(accept(ts,TK_LBRACK)){ Token *dm=peek(ts); expect(ts,TK_ICONST,"array extent"); gty.array_m=(int)dm->ival; expect(ts,TK_RBRACK,"]"); }
            }
            skip_register_suffix(ts);
            expect(ts,TK_SEMI,";");
            hpush((void**)&hp.globals,&hp.nglobals,&gcap,&gg,sizeof gg);
            continue;
        }
        hf.name=strdup(fn->text); hf.line=fn->line;
        advance(ts); /* consume the function name */
        expect(ts,TK_LPAREN,"(");
        HLSLParam *ps=NULL; size_t np=0,pcap=0;
        while(peek(ts)->kind!=TK_RPAREN){
            HLSLParam hp={0};
            if(accept(ts,TK_KW_INOUT))hp.inq=3;
            else if(accept(ts,TK_KW_OUT))hp.inq=2;
            else if(accept(ts,TK_KW_IN))hp.inq=1;
            /* qualifiers HLSL tolerates on params: uniform / const / static */
            while(peek(ts)->kind==TK_KW_UNIFORM||peek(ts)->kind==TK_KW_CONSTANT||peek(ts)->kind==TK_KW_STATIC) advance(ts);
            while(peek(ts)->kind==TK_KW_LINEAR||peek(ts)->kind==TK_KW_NOPERSPECTIVE||
                  peek(ts)->kind==TK_KW_CENTROID||peek(ts)->kind==TK_KW_SAMPLE) advance(ts);
            hp.ty=hlsl_type(ts);
            Token *pn=peek(ts); expect_name(ts,"param name");
            hp.name=strdup(pn->text);
            if(accept(ts,TK_COLON)){ Token *st=peek(ts); expect(ts,TK_IDENT,"semantic after :"); hp.sem=strdup(st->text); }
            if(accept(ts,TK_LBRACK)){
                Token *dn=peek(ts); expect(ts,TK_ICONST,"array extent"); hp.ty.array_n=(int)dn->ival; expect(ts,TK_RBRACK,"]");
            }
            hpush((void**)&ps,&np,&pcap,&hp,sizeof hp);
            if(!accept(ts,TK_COMMA))break;
        }
        expect(ts,TK_RPAREN,")");
        hf.params=ps; hf.np=np;
        if(accept(ts,TK_COLON)){ Token *st=peek(ts); expect(ts,TK_IDENT,"return semantic"); hf.ret_sem=strdup(st->text); }
        if(accept(ts,TK_SEMI)){ /* function prototype (forward declaration): skip */
            for(size_t q=0;q<np;q++)free(ps[q].name); free(ps); continue; }
        expect(ts,TK_LBRACE,"{");
        hf.body=parse_braced(ts);
        hpush((void**)&hp.funcs,&hp.nfuncs,&fcap,&hf,sizeof hf);
    }
    return hp;
}
