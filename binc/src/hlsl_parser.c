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
        /* ByteAddressBuffer -> raw uint device buffer; the struct_name marker
         * distinguishes it from StructuredBuffer<uint> (atomic-capable) */
        advance(ts); Type t={0}; t.kind=T_UINT32; t.is_ptr=1; t.as=AS_DEVICE;
        t.struct_name=strdup("$byteaddr"); return t;
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
            int vals[3]={1,1,1};
            for(int v=0;v<3;v++){
                if(peek(ts)->kind==TK_ICONST){ vals[v]=(int)peek(ts)->ival; advance(ts); }
                else if(peek(ts)->kind==TK_FCONST){ vals[v]=(int)peek(ts)->fval; advance(ts); }
                else { /* non-literal (macro from a stripped include): warn + skip */
                    fprintf(stderr,"binc: warning: numthreads arg %d is not a literal — assuming 1\n",v+1);
                    while(peek(ts)->kind!=TK_COMMA&&peek(ts)->kind!=TK_RPAREN&&peek(ts)->kind!=TK_EOF) advance(ts);
                }
                if(v<2){ if(peek(ts)->kind==TK_COMMA) advance(ts); }
            }
            expect(ts,TK_RPAREN,")"); f->numtx=vals[0]; f->numty=vals[1]; f->numtz=vals[2]; f->has_numthreads=1;
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

/* optional `: register(bN)` / `: packoffset(cN.x)` suffix; returns the last
 * register slot (b=0,t=1,u=2,s=3,c=4 classes; -1 when absent).
 * HLSL allows several in a row: `float4 f : register(vs, c0) : register(ps, c1);` */
static int skip_register_suffix(TokStream *ts){
    int reg=-1;
    for(;;){
        if(!accept(ts,TK_COLON)) return reg;
        if(accept(ts,TK_KW_REGISTER)){ expect(ts,TK_LPAREN,"(");
            if(peek(ts)->kind==TK_IDENT){ /* register(vs, ...) / register(ps, ...): ignore */
                Token *r=peek(ts);
                if(!strcmp(r->text,"b")||!strcmp(r->text,"t")||!strcmp(r->text,"u")||!strcmp(r->text,"s")||!strcmp(r->text,"c")){
                    advance(ts);
                    if(peek(ts)->kind==TK_ICONST){ reg=(int)peek(ts)->ival; advance(ts); }
                }
            }
            while(peek(ts)->kind!=TK_RPAREN&&peek(ts)->kind!=TK_EOF) advance(ts);
            expect(ts,TK_RPAREN,")"); continue; }
        if(accept(ts,TK_KW_PACKOFFSET)){ expect(ts,TK_LPAREN,"(");
            while(peek(ts)->kind!=TK_RPAREN&&peek(ts)->kind!=TK_EOF) advance(ts);
            expect(ts,TK_RPAREN,")"); continue; }
        /* bare semantic-ish ident: consume it (and a macro-form `IDENT(...)`
         * like UAV_REGISTER(x) when the include that defined it is missing);
         * loop for `: SEMANTIC : register(c0)` */
        if(peek(ts)->kind!=TK_SEMI&&peek(ts)->kind!=TK_EOF){
            advance(ts);
            if(peek(ts)->kind==TK_LPAREN){
                int d=0;
                do{ Token *t=peek(ts);
                    if(t->kind==TK_LPAREN)d++;
                    else if(t->kind==TK_RPAREN)d--;
                    if(t->kind==TK_EOF) break;
                    advance(ts); } while(d>0);
            }
        }
        continue;
    }
}

/* fold a constant global-initializer expression (binops/unary on literals and
 * earlier-declared const globals); returns the float value, sets *is_int and
 * *ival (the out params) for integer-only results */
static double fold_init(Expr *e, int *is_int, long *ival, HLSLGlobal *gs, size_t ngs){
    if(!e) return 0;
    switch(e->kind){
    case E_FCONST: *is_int=0; return e->fval;
    case E_ICONST: *is_int=1; *ival=e->ival; return (double)e->ival;
    case E_BOOL:   *is_int=1; *ival=e->bval; return (double)e->bval;
    case E_IDENT:  for(size_t i=0;i<ngs;i++) if(!strcmp(gs[i].name,e->name)&&gs[i].has_init){
                       *is_int=gs[i].is_int; *ival=gs[i].ival; return gs[i].fval; }
                   *is_int=1; *ival=0; return 0;
    case E_NEG: { double v=fold_init(e->operand,is_int,ival,gs,ngs); if(*is_int)*ival=-*ival; else return -v; return v; }
    case E_BIN: {
        int li,ri; long lv=0,rv=0;
        double l=fold_init(e->lhs,&li,&lv,gs,ngs);
        double r=fold_init(e->rhs,&ri,&rv,gs,ngs);
        double v=0; switch(e->bop){
            case B_ADD: v=l+r; if(li&&ri){*is_int=1;*ival=lv+rv;return v;} break;
            case B_SUB: v=l-r; if(li&&ri){*is_int=1;*ival=lv-rv;return v;} break;
            case B_MUL: v=l*r; if(li&&ri){*is_int=1;*ival=lv*rv;return v;} break;
            case B_DIV: v=l/r; break;
            case B_MOD: v=li&&ri?(double)(lv%rv):0; break;
            default: break;
        }
        *is_int=0; return v;
    }
    default: *is_int=1; *ival=0; return 0;
    }
}
static void hpush(void **arr, size_t *n, size_t *cap, void *item, size_t sz){    if(*n==*cap){*cap=*cap?*cap*2:8; *arr=realloc(*arr,*cap*sz);} 
    memcpy((char*)*arr+(*n)*sz,item,sz); (*n)++;
}

/* skip a `technique ... { ... }` block (the .fx effect system: pass blocks,
 * compile statements, render states — none of it survives to the GPU) */
static void skip_technique(TokStream *ts){
    advance(ts); /* the technique keyword */
    if(peek(ts)->kind==TK_IDENT||peek(ts)->kind==TK_KW_PASS) advance(ts); /* optional name */
    if(peek(ts)->kind==TK_IDENT&&!strcmp(peek(ts)->text,"Pass")) advance(ts); /* capital-P `Pass P0` */
    if(peek(ts)->kind==TK_LT){ /* optional D3D9 annotation: < ... > */
        int d=0;
        do{ Token *t=peek(ts);
            if(t->kind==TK_LT)d++;
            else if(t->kind==TK_GT)d--;
            if(t->kind==TK_EOF) die(t->line,"unterminated technique annotation");
            advance(ts); } while(d>0);
    }
    if(peek(ts)->kind==TK_LBRACE){
        int depth=0;
        for(;;){
            Token t=*peek(ts);
            if(t.kind==TK_EOF) die(t.line,"unterminated technique block");
            if(t.kind==TK_LBRACE) depth++;
            else if(t.kind==TK_RBRACE){ depth--; if(depth==0) break; }
            advance(ts);
        }
        advance(ts); /* consume the closing brace */
        if(peek(ts)->kind==TK_SEMI) advance(ts); /* `};` — the ; is optional */
    }
    /* technique name without a block: nothing to consume */
}

HLSLProg hlsl_parse(TokStream *ts){
    HLSLProg hp={0}; size_t fcap=0,scap=0,ccap=0,gcap=0;
    Program tmp={0};
    while(peek(ts)->kind!=TK_EOF){
        /* .fx effect system: `technique ... { pass ... { ... } }` blocks carry
         * only host-side render states — strip them entirely */
        if(peek(ts)->kind==TK_KW_TECHNIQUE||
           (peek(ts)->kind==TK_IDENT&&(!strcmp(peek(ts)->text,"technique10")||!strcmp(peek(ts)->text,"technique9")||!strcmp(peek(ts)->text,"technique11")||!strcmp(peek(ts)->text,"Technique")))){
            skip_technique(ts); continue; }
        /* leading attributes before a function */
        HLSLFunc attrs={0};
        parse_attributes(ts,&attrs);
        Token *kt=peek(ts);
        if(kt->kind==TK_SEMI){ advance(ts); continue; } /* stray top-level ; (UE emits ;; after structs) */
        if(kt->kind==TK_IDENT&&!strcmp(kt->text,"_Static_assert")){ /* C11 layout assertion: skip to ; */
            while(peek(ts)->kind!=TK_SEMI&&peek(ts)->kind!=TK_EOF) advance(ts);
            if(peek(ts)->kind==TK_SEMI) advance(ts);
            continue;
        }
        if(kt->kind==TK_IDENT&&!strcmp(kt->text,"_Pragma")){ /* DXC _Pragma("dxc ...") diagnostics */
            advance(ts); /* _Pragma */
            if(peek(ts)->kind==TK_LPAREN){ int d=0;
                do{ Token *t=peek(ts);
                    if(t->kind==TK_LPAREN)d++;
                    else if(t->kind==TK_RPAREN)d--;
                    if(t->kind==TK_EOF) die(t->line,"unterminated _Pragma");
                    advance(ts); } while(d>0); }
            continue;
        }
        if(kt->kind==TK_IDENT&&!strcmp(kt->text,"typedef")){ /* C-style typedef: skip to ; (uses fall back to generic struct typing) */
            while(peek(ts)->kind!=TK_SEMI&&peek(ts)->kind!=TK_EOF) advance(ts);
            if(peek(ts)->kind==TK_SEMI) advance(ts);
            continue;
        }
        if(kt->kind==TK_KW_TEMPLATE){
            /* HLSL2021 template decl: skip the template<...> header, then
             * parse the struct/function on the next iteration (tvar
             * references fall back to generic struct typing) */
            advance(ts); /* template */
            expect(ts,TK_LT,"<");
            int tdepth=1;
            while(tdepth>0&&peek(ts)->kind!=TK_EOF){
                Token *t=peek(ts);
                if(t->kind==TK_LT) tdepth++;
                else if(t->kind==TK_GT) tdepth--;
                advance(ts);
            }
            continue;
        }

        /* D3D9 effect globals: `sampler X = sampler_state {...};` and
         * `texture X <...> : register(...);` — the sampler_state block carries
         * host-side filter/wrap state; the texture registers as a module
         * global so tex2D-family calls can bind it */
        if((kt->kind==TK_KW_SAMPLER&&!(ts->i+2<ts->n&&ts->toks[ts->i+2].kind==TK_LPAREN))|| /* `SamplerState F(...)` is a function */
           (kt->kind==TK_IDENT&&(!strcmp(kt->text,"texture")||!strcmp(kt->text,"textureCUBE")||!strcmp(kt->text,"sampler2D")||!strcmp(kt->text,"samplerCUBE")||!strcmp(kt->text,"sampler3D")||!strcmp(kt->text,"sampler1D")||!strcmp(kt->text,"sampler2DShadow")||!strcmp(kt->text,"samplerCUBEShadow")))){
            if(getenv("BINC_DEBUG_D3D9")) fprintf(stderr,"DBG d3d9: %s at line %d\n",kt->text,kt->line);
            int is_samp=(kt->kind==TK_KW_SAMPLER)||(strcmp(kt->text,"texture")&&strcmp(kt->text,"textureCUBE"));
            advance(ts);
            Token *gn2=peek(ts); expect(ts,TK_IDENT,"D3D9 sampler/texture name");
            if(is_samp){
                char *texname=NULL;
                if(peek(ts)->kind==TK_LT){ /* D3D9 annotation before sampler_state: < ... > */
                    int d=0;
                    do{ Token *t=peek(ts);
                        if(t->kind==TK_LT)d++;
                        else if(t->kind==TK_GT)d--;
                        if(t->kind==TK_EOF) die(t->line,"unterminated sampler annotation");
                        advance(ts); } while(d>0);
                }
                if(accept(ts,TK_EQ)){
                    int d=0;
                    for(;;){ Token *t=peek(ts);
                        if(t->kind==TK_LBRACE)d++;
                        else if(t->kind==TK_RBRACE)d--;
                        else if(t->kind==TK_SEMI&&d==0) break;
                        if(t->kind==TK_IDENT&&!strcmp(t->text,"Texture")&&(ts->i+1<ts->n)){
                            /* sampler_state { Texture = <t0>; } — capture the bound texture
                             * (the workshop .fx files use `Texture = (Name);` too) */
                            size_t save=ts->i+2; /* skip `Texture =` */
                            if(save<ts->n&&(ts->toks[save].kind==TK_LT||ts->toks[save].kind==TK_LPAREN)) save++;
                            if(save<ts->n&&ts->toks[save].kind==TK_IDENT) texname=strdup(ts->toks[save].text);
                        }
                        if(t->kind==TK_EOF) die(t->line,"unterminated sampler_state");
                        advance(ts); }
                    /* bare `sampler s = <texture>` (no sampler_state) */
                    if(!texname&&(ts->i+1<ts->n)&&ts->toks[ts->i+1].kind==TK_LT){
                        size_t save=ts->i+2;
                        if(save<ts->n&&ts->toks[save].kind==TK_IDENT) texname=strdup(ts->toks[save].text);
                    }
                } else if(peek(ts)->kind==TK_LBRACE){ /* SamplerState X { ... }; */
                    int d=0;
                    do{ Token *t=peek(ts);
                        if(t->kind==TK_LBRACE)d++;
                        else if(t->kind==TK_RBRACE)d--;
                        if(t->kind==TK_EOF) die(t->line,"unterminated SamplerState block");
                        advance(ts); } while(d>0);
                }
                skip_register_suffix(ts); /* bare `sampler s0 : register(s0);` */
                expect(ts,TK_SEMI,";");
                /* register the sampler as a module global so Sample() can bind it;
                 * samplerCUBE marks the bound texture as a cube map */
                Type st={0}; st.kind=T_SAMPLER;
                if(!strcmp(kt->text,"samplerCUBE")||!strcmp(kt->text,"samplerCUBEShadow")) st.tex_cube=1;
                HLSLGlobal sg={0}; sg.name=strdup(gn2->text); sg.ty=st; sg.line=gn2->line;
                sg.tex_name=texname; /* may be NULL: bind by register index at codegen */
                hpush((void**)&hp.globals,&hp.nglobals,&gcap,&sg,sizeof sg);
                if(st.tex_cube&&texname){
                    /* mark the bound texture global as a cube map (its param type drives the AIR) */
                    for(size_t gi=0;gi<hp.nglobals;gi++)
                        if(hp.globals[gi].ty.kind==T_TEXTURE&&!strcmp(hp.globals[gi].name,texname))
                            hp.globals[gi].ty.tex_cube=1;
                }
            } else {
                skip_register_suffix(ts); /* `texture X : SEMANTIC < ... >;` */
                if(accept(ts,TK_LT)){ int d=1; /* the < is already consumed */
                    do{ Token *t=peek(ts);
                        if(t->kind==TK_LT)d++;
                        else if(t->kind==TK_GT)d--;
                        if(t->kind==TK_EOF) die(t->line,"unterminated texture declaration");
                        advance(ts); } while(d>0); }
                skip_register_suffix(ts);
                expect(ts,TK_SEMI,";");
                Type tt={0}; tt.kind=T_TEXTURE; tt.tex_elt=T_FLOAT;
                if(kt->kind==TK_IDENT&&!strcmp(kt->text,"textureCUBE")) tt.tex_cube=1;
                HLSLGlobal tg={0}; tg.name=strdup(gn2->text); tg.ty=tt; tg.line=gn2->line;
                hpush((void**)&hp.globals,&hp.nglobals,&gcap,&tg,sizeof tg);
            }
            continue;
        }

        /* D3D10 state objects: `DepthStencilState X { ... };` /
         * `BlendState X { ... };` / `RasterizerState X { ... };` — host-side */
        if(kt->kind==TK_IDENT&&(ts->i+1<ts->n)&&ts->toks[ts->i+1].kind==TK_IDENT&&
           !(ts->i+2<ts->n&&ts->toks[ts->i+2].kind==TK_LPAREN)&& /* `SamplerState F(...)` is a function */
           (!strcmp(kt->text,"DepthStencilState")||!strcmp(kt->text,"BlendState")||
            !strcmp(kt->text,"RasterizerState")||!strcmp(kt->text,"SamplerState"))){
            advance(ts); advance(ts); /* type + name */
            if(peek(ts)->kind==TK_LBRACE){
                int d=0;
                do{ Token *t=peek(ts);
                    if(t->kind==TK_LBRACE)d++;
                    else if(t->kind==TK_RBRACE)d--;
                    if(t->kind==TK_EOF) die(t->line,"unterminated state block");
                    advance(ts); } while(d>0);
            }
            if(peek(ts)->kind==TK_SEMI) advance(ts);
            continue;
        }

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
            cb.reg=skip_register_suffix(ts); /* cbuffer X : register(b0) { */
            expect(ts,TK_LBRACE,"{");
            Field *f=NULL; size_t n=0,cap=0;
            while(peek(ts)->kind!=TK_RBRACE&&peek(ts)->kind!=TK_EOF){
                Type ty=hlsl_type(ts);
                do{
                    Token *fn=peek(ts); expect(ts,TK_IDENT,"cbuffer field name");
                    char *sem=NULL;
                    if(peek(ts)->kind==TK_COLON&&(ts->i+1<ts->n)&&ts->toks[ts->i+1].kind==TK_IDENT){
                        advance(ts); /* ':' — semantic only; register/packoffset
                                      * are left for skip_register_suffix below */
                        Token *st=peek(ts); advance(ts); sem=strdup(st->text);
                    }
                    skip_register_suffix(ts); /* : register(c0) / : packoffset(c0.x) */
                    if(accept(ts,TK_LBRACK)){ /* array member [N] or [N][M] */
                        ty.array_n=parse_array_extent(ts); expect(ts,TK_RBRACK,"]");
                        if(accept(ts,TK_LBRACK)){ ty.array_m=parse_array_extent(ts); expect(ts,TK_RBRACK,"]"); }
                    }
                    skip_register_suffix(ts); /* suffix may follow the array: name[4] : packoffset(c2) */
                    if(accept(ts,TK_EQ)){ /* cbuffer field default value: parse + discard
                        (the host provides the real data; the default is a hint) */
                        if(peek(ts)->kind==TK_LBRACE){ int d=0;
                            do{ Token *t=peek(ts);
                                if(t->kind==TK_LBRACE)d++;
                                else if(t->kind==TK_RBRACE)d--;
                                if(t->kind==TK_EOF) die(t->line,"unterminated default initializer");
                                advance(ts); } while(d>0);
                        } else parse_expr(ts);
                    }
                    if(n==cap){cap=cap?cap*2:8;f=realloc(f,cap*sizeof(Field));}
                    f[n++]=(Field){strdup(fn->text),ty,0,0,sem};
                } while(accept(ts,TK_COMMA));
                expect(ts,TK_SEMI,";");
            }
            expect(ts,TK_RBRACE,"}");
            skip_register_suffix(ts);
            if(peek(ts)->kind==TK_SEMI) advance(ts); /* the ; after a cbuffer block is optional */
            cb.fields=f; cb.nfields=n;
            hpush((void**)&hp.cbufs,&hp.ncbufs,&ccap,&cb,sizeof cb);
            continue;
        }
        if(kt->kind==TK_KW_GROUPSHARED||kt->kind==TK_KW_STATIC||kt->kind==TK_KW_CONSTANT||kt->kind==TK_KW_UNIFORM){
            int gs=0, st=0, cn=0, un=0;
            for(;;){
                if(accept(ts,TK_KW_GROUPSHARED)) gs=1;
                else if(accept(ts,TK_KW_STATIC)) st=1;
                else if(accept(ts,TK_KW_CONSTANT)) cn=1;
                else if(accept(ts,TK_KW_UNIFORM)){ un=1; } /* SPIRV-Cross `uniform float4 x;` globals */
                else break;
            }
            (void)un; /* uniform globals fold into __uniforms (non-const scalars) */
            /* const/precise-qualified FUNCTION (`precise float F(...)`) falls
             * through to the function path — probe type+name+'(' */
            {
                size_t psave=ts->i;
                Type qty=hlsl_type(ts);
                if(peek(ts)->kind==TK_IDENT&&(ts->i+1<ts->n)&&ts->toks[ts->i+1].kind==TK_LPAREN){
                    ts->i=psave;
                    goto fn_path;
                }
                ts->i=psave;
            }
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
            gg.name=strdup(gn->text); gg.ty=ty; gg.is_groupshared=gs; gg.is_const=cn; gg.is_static=st; gg.line=gn->line;
            skip_register_suffix(ts); /* semantic/register may precede the annotation+init */
            if(peek(ts)->kind==TK_LT){ /* D3D9 effect annotation before the initializer: < ... > */
                int d=0;
                do{ Token *t=peek(ts);
                    if(t->kind==TK_LT)d++;
                    else if(t->kind==TK_GT)d--;
                    if(t->kind==TK_EOF) die(t->line,"unterminated annotation");
                    advance(ts); } while(d>0);
            }
            if(accept(ts,TK_LBRACK)){ /* global array [N] or [N][M] */
                gg.ty.array_n=parse_array_extent(ts); expect(ts,TK_RBRACK,"]");
                if(accept(ts,TK_LBRACK)){ gg.ty.array_m=parse_array_extent(ts); expect(ts,TK_RBRACK,"]"); }
            }
            if(accept(ts,TK_EQ)){
                gg.has_init=1;
                Expr *e=NULL;
                if(peek(ts)->kind==TK_LBRACE){ /* HLSL brace initializer: skip */
                    int depth=0;
                    do{ Token *t=peek(ts);
                        if(t->kind==TK_LBRACE)depth++;
                        else if(t->kind==TK_RBRACE)depth--;
                        if(t->kind==TK_EOF) die(t->line,"unterminated initializer");
                        advance(ts); } while(depth>0);
                } else e=parse_expr(ts);
                if(e&&e->kind==E_ICONST){ gg.is_int=1; gg.ival=e->ival; }
                else if(e&&e->kind==E_FCONST){ gg.fval=e->fval; }
                else if(e&&e->kind==E_BOOL){ gg.is_int=1; gg.ival=e->bval; }
                else if(e) gg.fval=fold_init(e,&gg.is_int,&gg.ival,hp.globals,hp.nglobals);
            }
            gg.reg=skip_register_suffix(ts);
            if(peek(ts)->kind==TK_LT){ /* D3D9 effect annotation: < ... > */
                int d=0;
                do{ Token *t=peek(ts);
                    if(t->kind==TK_LT)d++;
                    else if(t->kind==TK_GT)d--;
                    if(t->kind==TK_EOF) die(t->line,"unterminated annotation");
                    advance(ts); } while(d>0);
            }
            if(getenv("BINC_DEBUG_D3D9")) fprintf(stderr,"DBG global %s: next kind=%d '%s'\n",gg.name,peek(ts)->kind,peek(ts)->text);
            expect(ts,TK_SEMI,";");
            hpush((void**)&hp.globals,&hp.nglobals,&gcap,&gg,sizeof gg);
            continue;
        }
        /* function definition, or an unqualified module global
         * (StructuredBuffer<>, SamplerState, plain typed vars, ...) */
        fn_path:
        if(peek(ts)->kind==TK_IDENT&&!strcmp(peek(ts)->text,"inline")) advance(ts); /* HLSL `inline` qualifier */
        size_t save = ts->i;
        HLSLFunc hf=attrs; /* attributes (numthreads) carried over */
        hf.is_export=accept(ts,TK_KW_STATIC);
        accept(ts,TK_KW_IN); /* legal `in` before the return type */
        accept(ts,TK_KW_CONSTANT); /* `precise`/`const` return qualifier */
        hf.ret=hlsl_type(ts);
        Token *fn=peek(ts);
        if(getenv("BINC_DEBUG_D3D9")) fprintf(stderr,"DBG fnprobe: ret_kind=%d fn='%s' kind=%d next=%d\n",hf.ret.kind,fn->text,fn->kind,(ts->i+1<ts->n)?ts->toks[ts->i+1].kind:-1);
        int is_fn = fn->kind==TK_IDENT && (ts->i+1<ts->n) && ts->toks[ts->i+1].kind==TK_LPAREN;
        if(!is_fn){
            ts->i=save;
            Type gty=hlsl_type(ts);
            HLSLGlobal gg={0};
            Token *gn=peek(ts); expect(ts,TK_IDENT,"global name");
            if(peek(ts)->kind==TK_COLON&&(ts->i+1<ts->n)&&ts->toks[ts->i+1].kind==TK_COLON){
                /* C++-style out-of-line method: `RetType Struct::Method(args) { body }`
                 * — UE defines struct methods out-of-line in .ush files. Skip it. */
                advance(ts); advance(ts); /* :: */
                if(peek(ts)->kind==TK_IDENT) advance(ts); /* method name */
                if(peek(ts)->kind==TK_LPAREN){
                    int d=0;
                    do{ Token *t=peek(ts);
                        if(t->kind==TK_LPAREN)d++;
                        else if(t->kind==TK_RPAREN)d--;
                        if(t->kind==TK_EOF) die(t->line,"unterminated method signature");
                        advance(ts); } while(d>0);
                }
                if(peek(ts)->kind==TK_LBRACE){
                    int bd=0;
                    do{ Token *t=peek(ts);
                        if(t->kind==TK_LBRACE)bd++;
                        else if(t->kind==TK_RBRACE)bd--;
                        if(t->kind==TK_EOF) die(t->line,"unterminated method body");
                        advance(ts); } while(bd>0);
                } else if(peek(ts)->kind==TK_SEMI) advance(ts);
                continue;
            }
            gg.name=strdup(gn->text); gg.ty=gty; gg.line=gn->line;
            skip_register_suffix(ts); /* semantic/register may precede the annotation+init */
            if(peek(ts)->kind==TK_LT){ /* D3D9 effect annotation before the initializer: < ... > */
                int d=0;
                do{ Token *t=peek(ts);
                    if(t->kind==TK_LT)d++;
                    else if(t->kind==TK_GT)d--;
                    if(t->kind==TK_EOF) die(t->line,"unterminated annotation");
                    advance(ts); } while(d>0);
            }
            if(accept(ts,TK_LBRACK)){ /* global array [N] or [N][M] */
                gg.ty.array_n=parse_array_extent(ts); expect(ts,TK_RBRACK,"]");
                if(accept(ts,TK_LBRACK)){ gg.ty.array_m=parse_array_extent(ts); expect(ts,TK_RBRACK,"]"); }
            }
            if(accept(ts,TK_EQ)){
                gg.has_init=1;
                Expr *e=NULL;
                if(peek(ts)->kind==TK_LBRACE){ /* HLSL brace initializer: skip */
                    int depth=0;
                    do{ Token *t=peek(ts);
                        if(t->kind==TK_LBRACE)depth++;
                        else if(t->kind==TK_RBRACE)depth--;
                        if(t->kind==TK_EOF) die(t->line,"unterminated initializer");
                        advance(ts); } while(depth>0);
                } else e=parse_expr(ts);
                if(e&&e->kind==E_ICONST){ gg.is_int=1; gg.ival=e->ival; }
                else if(e&&e->kind==E_FCONST){ gg.fval=e->fval; }
                else if(e&&e->kind==E_BOOL){ gg.is_int=1; gg.ival=e->bval; }
                else if(e) gg.fval=fold_init(e,&gg.is_int,&gg.ival,hp.globals,hp.nglobals);
            }
            gg.reg=skip_register_suffix(ts);
            if(peek(ts)->kind==TK_LT){ /* D3D9 effect annotation: < ... > */
                int d=0;
                do{ Token *t=peek(ts);
                    if(t->kind==TK_LT)d++;
                    else if(t->kind==TK_GT)d--;
                    if(t->kind==TK_EOF) die(t->line,"unterminated annotation");
                    advance(ts); } while(d>0);
            }
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
            /* in/out/inout and const/static/uniform interleave (`const in float3 P`) */
            for(;;){
                if(peek(ts)->kind==TK_KW_INOUT&&!hp.inq){ hp.inq=3; advance(ts); }
                else if(peek(ts)->kind==TK_KW_OUT){ if(!hp.inq) hp.inq=2; else if(hp.inq==1) hp.inq=3; advance(ts); } /* `in out` = inout */
                else if(peek(ts)->kind==TK_KW_IN&&!hp.inq){ hp.inq=1; advance(ts); }
                else if(peek(ts)->kind==TK_KW_UNIFORM){ hp.is_uniform=1; advance(ts); }
                else if(peek(ts)->kind==TK_KW_CONSTANT||peek(ts)->kind==TK_KW_STATIC) advance(ts);
                else break;
            }
            while(peek(ts)->kind==TK_KW_LINEAR||peek(ts)->kind==TK_KW_NOPERSPECTIVE||
                  peek(ts)->kind==TK_KW_CENTROID||peek(ts)->kind==TK_KW_SAMPLE) advance(ts);
            /* unknown qualifier macro (config-gated off, e.g.
             * INPUT_POSITION_QUALIFIERS) before a keyword type: skip it */
            if(peek(ts)->kind==TK_IDENT&&(ts->i+1<ts->n)){
                TokKind nk=ts->toks[ts->i+1].kind;
                if(nk==TK_KW_FLOAT||nk==TK_KW_HALF||nk==TK_KW_INT||nk==TK_KW_UINT||nk==TK_KW_BOOL||nk==TK_KW_MAT)
                    advance(ts);
            }
            /* GS input primitives: point / line / lineadj / triangle /
             * triangleadj — the primitive keyword precedes the struct type */
            while(peek(ts)->kind==TK_IDENT&&
                  (!strcmp(peek(ts)->text,"triangleadj")||!strcmp(peek(ts)->text,"lineadj")||
                   !strcmp(peek(ts)->text,"triangle")||!strcmp(peek(ts)->text,"line")||
                   !strcmp(peek(ts)->text,"point")||!strcmp(peek(ts)->text,"quad")||
                   !strcmp(peek(ts)->text,"triangle_adj")||!strcmp(peek(ts)->text,"line_adj")))
                advance(ts);
            hp.ty=hlsl_type(ts);
            Token *pn=peek(ts); expect_name(ts,"param name");
            hp.name=strdup(pn->text);
            if(accept(ts,TK_LBRACK)){
                hp.ty.array_n=parse_array_extent(ts); expect(ts,TK_RBRACK,"]");
            }
            if(accept(ts,TK_COLON)){ Token *st=peek(ts); expect(ts,TK_IDENT,"semantic after :"); hp.sem=strdup(st->text); }
            /* HLSL default arguments (`float DepthC = 0.0f`): keep the
             * expression so calls omitting a defaulted arg can fill it in */
            if(accept(ts,TK_EQ)){
                hp.def=parse_expr(ts);
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
        if(getenv("BINC_DEBUG_D3D9")&&!strcmp(hf.name,"IsNonZeroFast")) fprintf(stderr,"DBG fnreg: %s nparams=%zu line=%d\n",hf.name,hf.np,hf.line);
    }
    return hp;
}
