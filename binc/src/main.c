/* main.c — binc driver: read .binc, lex, parse, emit AIR .ll, drive metal/metallib -> .metallib */
#include "binc.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>

#define BINC_VERSION "0.2.0"

static const char *usage_text =
    "usage: binc <file.binc> [options]\n"
    "options:\n"
    "  -o <file.metallib>   output metallib path (default: <base>.metallib)\n"
    "  --emit-ll            stop after emitting AIR .ll (no metal/metallib)\n"
    "  -h, --help           show this help and exit\n"
    "  --version            show the compiler version and exit\n"
    "  -I <dir>             add an include search path\n"
    "  -no-prelude          disable the automatic prelude include\n"
    "  -i                   interpret on the CPU (scalar/vector subset, no GPU)\n"
    "  -fsyntax-only        parse and type-check only; no AIR, metal, or output\n"
    "environment: METAL / METALLIB override the AIR tool invocations\n"
    "             (defaults: \"xcrun metal\" / \"xcrun metallib\")\n";

/* METAL/METALLIB env override, defaulting to xcrun (works on local and CI
 * macOS runners regardless of which Xcode is selected). */
static const char *tool(const char *env, const char *def){
    const char *v = getenv(env);
    return v && *v ? v : def;
}

/* ---- textual include preprocessor ----
 * `include "path";` splices another .binc file (resolved relative to the
 * including file, then -I dirs, then cwd); `once;` marks a file as included
 * at most once per compilation; include cycles are located errors. */
static char *inc_dirs[32]; static size_t ninc_dirs;
static void add_inc_dir(const char *d){ if(ninc_dirs<32) inc_dirs[ninc_dirs++]=strdup(d); }
static char *inc_stack[256]; static int ninc_stack;
static char *once_files[64]; static size_t n_once;

static char *dirname_of(const char *path){
    const char *slash = strrchr(path, '/');
    if(!slash) return strdup(".");
    char *d = malloc((size_t)(slash-path)+1);
    memcpy(d, path, (size_t)(slash-path)); d[slash-path]='\0';
    return d;
}
static void norm_path(char *p){
    /* collapse /./ and /../ segments in place so two spellings of the same
     * file (e.g. "DoubleFloat.ush" vs "../DoubleFloat.ush" vs
     * "/Engine/Private/DoubleFloat.ush") compare equal in is_once() */
    char *dst=p; const char *s=p;
    if(*s=='/'){ *dst++='/'; s++; }
    while(*s){
        while(*s=='/') s++;
        if(!*s) break;
        const char *e=s; while(*e&&*e!='/') e++;
        size_t l=(size_t)(e-s);
        if(l==1&&s[0]=='.'){ s=e; continue; }
        if(l==2&&s[0]=='.'&&s[1]=='.'){
            if(dst>p+1){ dst--; while(dst>p+1&&dst[-1]!='/') dst--; }
            else if(dst==p){ /* leading .. with nothing to collapse: keep it verbatim */
                *dst++='.'; *dst++='.'; }
            s=e; continue;
        }
        if(dst!=p&&dst[-1]!='/') *dst++='/';
        memcpy(dst,s,l); dst+=l;
        s=e;
    }
    *dst='\0';
}
static char *resolve_include(const char *curdir, const char *want){
    if(curdir && want[0]!='/' && want[0]!='\\'){ char p[1024]; snprintf(p,sizeof p,"%s/%s",curdir,want);
        if(!access(p,R_OK)){ norm_path(p); return strdup(p); } }
    /* UE virtual include paths: `/Engine/Public/X.ush` lives at
     * <inc_dir>/Engine/Shaders/Public/X.ush (ShaderCompilerWorker mapping) */
    if(!strncmp(want,"/Engine/",8)){
        for(size_t i=0;i<ninc_dirs;i++){ char p[1024]; snprintf(p,sizeof p,"%s/Engine/Shaders/%s",inc_dirs[i],want+8);
            if(!access(p,R_OK)){ norm_path(p); return strdup(p); } }
    }
    for(size_t i=0;i<ninc_dirs;i++){ char p[1024]; snprintf(p,sizeof p,"%s/%s",inc_dirs[i],want);
        if(!access(p,R_OK)){ norm_path(p); return strdup(p); } }
    if(!access(want,R_OK)){ char p[1024]; snprintf(p,sizeof p,"%s",want); norm_path(p); return strdup(p); }
    return NULL;
}
static int is_once(const char *path){ for(size_t i=0;i<n_once;i++) if(!strcmp(once_files[i],path)) return 1; return 0; }
static void mark_once(const char *path){ if(n_once<64) once_files[n_once++]=strdup(path); }

typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n){ if(b->n+n+1>b->cap){ b->cap=(b->cap?b->cap*2:8192); while(b->cap<b->n+n+1)b->cap*=2; b->p=realloc(b->p,b->cap); } memcpy(b->p+b->n,s,n); b->n+=n; b->p[b->n]='\0'; }

/* returns the spliced text of one file, or NULL after reporting an error */
static char *splice_file(const char *path){
    for(int i=0;i<ninc_stack-1;i++) if(!strcmp(inc_stack[i],path)){
        fprintf(stderr,"binc: error: include cycle: %s -> %s\n",inc_stack[ninc_stack-1],path);
        return NULL; }
    FILE *f=fopen(path,"rb"); if(!f){ fprintf(stderr,"binc: error: cannot open include file %s\n",path); return NULL; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char *text=malloc(n+1); if(n)fread(text,1,n,f); text[n]='\0'; fclose(f);
    if(n>=3&&(unsigned char)text[0]==0xEF&&(unsigned char)text[1]==0xBB&&(unsigned char)text[2]==0xBF) memmove(text,text+3,n-2); /* UTF-8 BOM */
    char *dir=dirname_of(path);
    Buf out={0}; long line=0;
    /* iterate lines preserving blanks (strtok_r would collapse them and shift
     * every subsequent diagnostic line) */
    char *ls=text;
    for(char *p=text;;p++){
        if(*p!='\n'&&*p!='\0') continue;
        int is_last=(*p=='\0');
        *p='\0';
        char *ln=ls; line++;
        char *t=ln; while(*t==' '||*t=='\t')t++;
        if(!strncmp(t,"once;",5)){ mark_once(path); }
        else if(!strncmp(t,"#pragma",7)){
            char *pq=t+7; while(*pq==' '||*pq=='\t')pq++;
            if(!strncmp(pq,"once",4)&&!(isalnum((unsigned char)pq[4])||pq[4]=='_')) mark_once(path);
            bput(&out,ln,strlen(ln)); bput(&out,"\n",1);
        }
        else if(!strncmp(t,"include ",8)){
            char *q=t+8; while(*q==' '||*q=='\t')q++;
            if(*q!='"'){ fprintf(stderr,"binc: error (%s line %ld): include needs \"path\";\n",path,line); free(out.p); return NULL; }
            q++; char *end=strchr(q,'"'); if(!end){ fprintf(stderr,"binc: error (%s line %ld): unterminated include path\n",path,line); free(out.p); return NULL; }
            *end='\0'; char *want=q; char *semi=end+1; while(*semi==' '||*semi=='\t')semi++;
            if(*semi!=';'){ fprintf(stderr,"binc: error (%s line %ld): include needs a trailing ;\n",path,line); free(out.p); return NULL; }
            char *res=resolve_include(dir,want);
            if(!res){ fprintf(stderr,"binc: error (%s line %ld): cannot find include file \"%s\"\n",path,line,want); free(out.p); return NULL; }
            if(!is_once(res)){
                inc_stack[ninc_stack++]=res;
                char *sub=splice_file(res);
                ninc_stack--;
                if(!sub){ free(res); free(out.p); return NULL; }
                bput(&out,sub,strlen(sub)); free(sub);
            }
            free(res);
        } else { bput(&out,ln,strlen(ln)); bput(&out,"\n",1); }
        if(is_last) break;
        ls=p+1;
    }
    free(text); free(dir);
    return out.p;
}

/* prelude auto-include: search argv[0] dir, -I dirs, then cwd */
/* ---- minimal preprocessor conditionals (UE .usf/.ush use #if heavily) ---- */
typedef struct { char *name; char *val; char params[8][64]; int nparams; } Def;

/* substitute one logical line with the LIVE defs table: object-like (whole
 * word) + function-like NAME(args) expansion with `##` pasting. Repeats
 * until stable (an expansion may introduce another macro's name). This runs
 * during pp_process so `#define/#undef` scoping is position-correct
 * (`#define FDFType FDFVector3 ... #undef FDFType` instantiations).
 *
 * C99 6.10.3.4 self-reference suppression: when a macro's expansion contains
 * the macro's OWN name, that residue is painted and never re-expanded (UE:
 * `#define UseBasePassSkylight OpaqueBasePass.Shared.UseBasePassSkylight`).
 * Without this, the multi-round rescan re-expands it until the round cap
 * (16 nested copies in the pp output). Track suppressed names per line. */
static int body_has_word(const char *body, const char *name){
    size_t nl=strlen(name);
    for(const char *b=body; (b=strstr(b,name)); b+=nl){
        int lb=b==body||!(isalnum((unsigned char)b[-1])||b[-1]=='_');
        int rb=!(isalnum((unsigned char)b[nl])||b[nl]=='_');
        if(lb&&rb) return 1;
    }
    return 0;
}
static int name_is_suppressed(const char *const *sup, int ns, const char *name){
    for(int i=0;i<ns;i++) if(!strcmp(sup[i],name)) return 1;
    return 0;
}
static void suppress_name(const char **sup, int *ns, const char *name){
    if(*ns>=16||name_is_suppressed(sup,*ns,name)) return;
    sup[(*ns)++] = name; /* defs table outlives the line: point, don't copy */
}
static char *pp_subst_line(const char *line, const Def *defs, size_t ndefs){
    Buf out={0}; const char *q=line;
    int changed=1; int ever=0;
    const char *suppressed[16]; int nsup=0;
    for(int round=0; changed&&round<16; round++){
        Buf nxt={0}; int did=0;
        if(getenv("BINC_DUMP_PP")&&strstr(line,"INVARIANT_ADD(INVARIANT_SUB")) fprintf(stderr,"DBG sub-round: r=%d q='%.120s'\n",round,q?q:"(null)");
        for(size_t d=0;d<ndefs;d++){
            size_t nl2=strlen(defs[d].name);
            if(!nl2) continue;
            if(name_is_suppressed(suppressed,nsup,defs[d].name)) continue;
            Buf o2={0};
            /* after a substitution emptied the line (did=1, nxt.p=NULL), keep
             * it empty — only fall back to the original line before any
             * substitution in this round (the very first iteration) */
            const char *s = nxt.p ? nxt.p : (did ? "" : q);
            if(defs[d].nparams>0){
                while(s&&*s){
                    const char *hit=strstr(s,defs[d].name);
                    if(!hit){ bput(&o2,s,strlen(s)); break; }
                    int lb=hit==s||!(isalnum((unsigned char)hit[-1])||hit[-1]=='_');
                    /* C99 permits whitespace between a function-like macro name
                     * and its opening parenthesis (UE uses `NAME  (...)`). */
                    const char *call=hit+nl2; while(*call&&isspace((unsigned char)*call)) call++;
                    if(!lb||*call!='('){ bput(&o2,s,(size_t)(hit-s+nl2)); s=hit+nl2; continue; }
                    const char *p=call+1; int dep=1; const char *argstart=p; int found=0;
                    char argbuf[8][128]; int nargs=0;
                    for(; *p; p++){
                        if(*p=='(') dep++;
                        else if(*p==')'){
                            dep--;
                            if(dep==0){ if(nargs<8){ size_t al=(size_t)(p-argstart); if(al>=128) al=127; memcpy(argbuf[nargs],argstart,al); argbuf[nargs][al]=0; nargs++; } found=1; break; }
                        }
                        else if(*p==','&&dep==1){ if(nargs<8){ size_t al=(size_t)(p-argstart); if(al>=128) al=127; memcpy(argbuf[nargs],argstart,al); argbuf[nargs][al]=0; nargs++; } argstart=p+1; }
                    }
                    if(!found){ bput(&o2,s,(size_t)(p-s)); break; }
                    if(nargs!=defs[d].nparams){ bput(&o2,s,(size_t)(p+1-s)); s=p+1; continue; }
                    char body[16384]; snprintf(body,sizeof body,"%s",defs[d].val);
                    for(int ai=0;ai<nargs;ai++){
                        size_t pl=strlen(defs[d].params[ai]);
                        Buf b2={0}; char *bq=body;
                        while(bq&&*bq){
                            char *bh=strstr(bq,defs[d].params[ai]);
                            if(!bh){ bput(&b2,bq,strlen(bq)); break; }
                            int bl=bh==bq||!(isalnum((unsigned char)bh[-1])||bh[-1]=='_');
                            int br=!(isalnum((unsigned char)bh[pl])||bh[pl]=='_');
                            if(bh[pl]=='#'&&bh[pl+1]=='#'){
                                bput(&b2,bq,(size_t)(bh-bq)); bput(&b2,argbuf[ai],strlen(argbuf[ai]));
                                bq=bh+pl+2;
                            } else if(pl>=2&&(bh-bq)>=2&&bh[-2]=='#'&&bh[-1]=='#'&&bl){
                                if(b2.n>=2) b2.n-=2; b2.p[b2.n]=0; bput(&b2,argbuf[ai],strlen(argbuf[ai]));
                                bq=bh+pl;
                            } else if(bl&&br){
                                bput(&b2,bq,(size_t)(bh-bq)); bput(&b2,argbuf[ai],strlen(argbuf[ai])); bq=bh+pl;
                            } else { bput(&b2,bq,(size_t)(bh-bq+pl)); bq=bh+pl; }
                        }
                        snprintf(body,sizeof body,"%s",b2.p?b2.p:""); free(b2.p);
                    }
                    bput(&o2,s,(size_t)(hit-s)); bput(&o2,body,strlen(body));
                    /* C99 6.10.3.4: only the macro's name in its OWN replacement
                     * list is painted — check val, NOT the arg-substituted body
                     * (an arg containing the name is caller text, not a residue) */
                    if(body_has_word(defs[d].val,defs[d].name)) suppress_name(suppressed,&nsup,defs[d].name);
                    s=p+1; did=1;
                }
            } else {
                while(s&&*s){
                    const char *hit=strstr(s,defs[d].name);
                    if(!hit){ bput(&o2,s,strlen(s)); break; }
                    int lb=hit==s||!(isalnum((unsigned char)hit[-1])||hit[-1]=='_');
                    int rb=!(isalnum((unsigned char)hit[nl2])||hit[nl2]=='_');
                    if(getenv("BINC_DUMP_PP")&&!strcmp(defs[d].name,"CALL_SITE_DEBUGLOC")) fprintf(stderr,"DBG obj: hit=%ld lb=%d rb=%d nl2=%zu c='%c'\n",(long)(hit-s),lb,rb,nl2,hit[nl2]?hit[nl2]:'0');
                    if(getenv("BINC_DUMP_PP")&&!strcmp(defs[d].name,"OPTIONAL_IsFrontFace")) fprintf(stderr,"DBG opt: hit=%ld lb=%d rb=%d nl2=%zu\n",(long)(hit-s),lb,rb,nl2);
                    if(lb&&rb){ bput(&o2,s,(size_t)(hit-s)); bput(&o2,defs[d].val,strlen(defs[d].val));
                        if(body_has_word(defs[d].val,defs[d].name)) suppress_name(suppressed,&nsup,defs[d].name);
                        s=hit+nl2; did=1; }
                    else { bput(&o2,s,(size_t)(hit-s+nl2)); s=hit+nl2; }
                }
            }
            free(nxt.p); nxt=o2;
        }
        changed = did; /* a substitution fired this round — rescan (empty results included) */
        if(did) ever=1;
        if(!nxt.p){ free(out.p); return ever?strdup(""):NULL; }
        free(out.p); out=nxt; q=out.p;
        if(!did&&out.p) break; /* no substitution in the whole round: stable */
    }
    return out.p?out.p:strdup("");
}

static const Def *pp_find(const Def *defs, size_t nd, const char *name){
    for(size_t i=0;i<nd;i++) if(!strcmp(defs[i].name,name)) return &defs[i];
    return NULL;
}
static long pp_val(const Def *defs, size_t nd, const char *nm){
    const Def *d=pp_find(defs,nd,nm);
    if(!d) return 0;
    return strtol(d->val,NULL,0);
}
/* recursive-descent over: ident | number | defined(ident)|defined ident | ! - ( ) && || == != < > <= >= + - * / % */
typedef struct { const char *p; const Def *defs; size_t nd; } PP;
static long pp_or(PP *pp);
static int pp_ident(PP *pp, char *out, size_t n){
    const char *s=pp->p; size_t i=0;
    while((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||*s=='_'||(i&&*s>='0'&&*s<='9')){
        if(i+1<n) out[i]=*s; i++; s++;
    }
    if(!i) return 0; out[i<n?i:n-1]='\0'; pp->p=s; return 1;
}
static long pp_unary(PP *pp){
    while(*pp->p==' '||*pp->p=='\t') pp->p++;
    if(*pp->p=='!'){ pp->p++; return !pp_unary(pp); }
    if(*pp->p=='-'){ pp->p++; return -pp_unary(pp); }
    if(*pp->p=='('){ pp->p++; long v=pp_or(pp); while(*pp->p==' '||*pp->p=='\t')pp->p++; if(*pp->p==')')pp->p++; return v; }
    if(!strncmp(pp->p,"defined",7)){
        const char *s=pp->p+7; while(*s==' '||*s=='\t')s++;
        int paren=0; if(*s=='('){ paren=1; s++; while(*s==' '||*s=='\t')s++; }
        char nm[128]; const char *save=pp->p; pp->p=s;
        int ok=pp_ident(pp,nm,sizeof nm);
        pp->p=save;
        if(ok){ long v=pp_find(pp->defs,pp->nd,nm)?1:0;
            pp->p=s+strlen(nm); if(paren){ while(*pp->p==' '||*pp->p=='\t')pp->p++; if(*pp->p==')')pp->p++; }
            return v; }
        return 0;
    }
    if(*pp->p>='0'&&*pp->p<='9'){ char *e; long v=strtol(pp->p,&e,0); pp->p=e; return v; }
    char nm[128];
    if(pp_ident(pp,nm,sizeof nm)) return pp_val(pp->defs,pp->nd,nm);
    return 0;
}
static long pp_mul(PP *pp){ long v=pp_unary(pp);
    for(;;){ while(*pp->p==' '||*pp->p=='\t')pp->p++;
        char op=*pp->p; if(op!='*'&&op!='/'&&op!='%') return v; pp->p++;
        long r=pp_unary(pp); v = op=='*'?v*r : op=='/'?(r?v/r:0) : (r?v%r:0); } }
static long pp_add(PP *pp){ long v=pp_mul(pp);
    for(;;){ while(*pp->p==' '||*pp->p=='\t')pp->p++;
        char op=*pp->p; if(op!='+'&&op!='-') return v; pp->p++;
        long r=pp_mul(pp); v = op=='+'?v+r:v-r; } }
static long pp_rel(PP *pp){ long v=pp_add(pp);
    for(;;){ while(*pp->p==' '||*pp->p=='\t')pp->p++;
        if(!strncmp(pp->p,"<=",2)){ pp->p+=2; v = v<=pp_add(pp); }
        else if(!strncmp(pp->p,">=",2)){ pp->p+=2; v = v>=pp_add(pp); }
        else if(*pp->p=='<'){ pp->p++; v = v<pp_add(pp); }
        else if(*pp->p=='>'){ pp->p++; v = v>pp_add(pp); }
        else return v; } }
static long pp_eq(PP *pp){ long v=pp_rel(pp);
    for(;;){ while(*pp->p==' '||*pp->p=='\t')pp->p++;
        if(!strncmp(pp->p,"==",2)){ pp->p+=2; v = v==pp_rel(pp); }
        else if(!strncmp(pp->p,"!=",2)){ pp->p+=2; v = v!=pp_rel(pp); }
        else return v; } }
static long pp_and(PP *pp){ long v=pp_eq(pp);
    for(;;){ while(*pp->p==' '||*pp->p=='\t')pp->p++;
        if(!strncmp(pp->p,"&&",2)){ pp->p+=2; long r=pp_eq(pp); v = v&&r; } else return v; } }
static long pp_or(PP *pp){ long v=pp_and(pp);
    for(;;){ while(*pp->p==' '||*pp->p=='\t')pp->p++;
        if(!strncmp(pp->p,"||",2)){ pp->p+=2; long r=pp_and(pp); v = v||r; } else return v; } }
static long pp_eval(const char *expr, const Def *defs, size_t nd){
    PP pp={expr,defs,nd}; return pp_or(&pp);
}

/* recursive HLSL preprocessor: #if/#ifdef/#ifndef/#elif/#else/#endif (evaluated
 * over the collected defines), #define (active branches only), #include
 * (active branches only — spliced recursively with once/cycle guards),
 * #pragma once (include-once marker), everything else dropped. Non-# lines
 * pass through when the enclosing branch is active. UE .usf/.ush rely on all
 * of this (6.7k #if blocks, 4.3k includes, no include semicolons). */
static void pp_process(const char *path, const char *src, Buf *out, Def *defs, size_t *ndefs){
    char *dir=dirname_of(path);
    int pp_stack[64]; int pp_depth=0; /* stack of (active<<1|taken) */
    const char *ls=src;
    while(ls&&*ls){
        /* logical line: join backslash line-continuations (UE multiline macros) */
        char linebuf[16384]; size_t llen=0;
        const char *cur=ls;
        for(;;){
            const char *nl2=strchr(cur,'\n');
            size_t l2=nl2?(size_t)(nl2-cur):strlen(cur);
            /* line-continuation: trailing `\` (or `\` before CRLF) */
            int has_cont = l2>0&&nl2&&(cur[l2-1]=='\\'||(l2>1&&cur[l2-2]=='\\'&&cur[l2-1]=='\r'));
            size_t take = has_cont ? l2-(cur[l2-1]=='\\'?1:2) : l2;
            if(llen+take<sizeof linebuf-1){ memcpy(linebuf+llen,cur,take); llen+=take; }
            if(!has_cont){ ls = nl2?nl2+1:NULL; break; }
            cur = nl2+1;
        }
        linebuf[llen]='\0';
        const char *L=linebuf; size_t len=llen;
        const char *first=L; while(*first==' '||*first=='\t'||*first=='\r') first++;
        int is_hash = len&&*first=='#';
        int active = pp_depth==0 || ((pp_stack[pp_depth-1]>>1)&1);
        if(is_hash){
            char *ln=strndup(first,len-(size_t)(first-ls));
            char *p=ln+1; while(*p==' '||*p=='\t') p++;
            /* UE writes `#if\tX`, `#define\tX` (tab after the directive word).
             * Normalize the whole directive line so the word matchers below
             * see `#if X` / `#define X` (values may contain tabs — harmless). */
            for(char *t=p;*t;t++) if(*t=='\t') *t=' ';
            if(!strncmp(p,"ifdef ",6)||!strncmp(p,"ifndef ",7)){
                int neg = p[2]=='n'; /* "ifndef": i-f-n-d-e-f — the 'n' is at p[2] */
                char *nm=p+(neg?7:6); while(*nm==' '||*nm=='\t')nm++;
                char *e=nm; while(*e&&*e!=' '&&*e!='\t'&&*e!='\r') e++; *e=0;
                long c = pp_find(defs,*ndefs,nm)?1:0; if(neg) c=!c;
                if(getenv("BINC_DUMP_PP")&&!strcmp(nm,"SM6_PROFILE")) fprintf(stderr,"DBG ifndef SM6_PROFILE: found=%ld -> c=%ld ndefs=%zu\n",pp_find(defs,*ndefs,nm)?1:0,c,*ndefs);
                int en = active&&c;
                if(pp_depth<64) pp_stack[pp_depth++]=(en<<1)|en;
            } else if(!strncmp(p,"if ",3)){
                long c=pp_eval(p+3,defs,*ndefs);
                int en = active&&c;
                if(pp_depth<64) pp_stack[pp_depth++]=(en<<1)|en;
            } else if(!strncmp(p,"elif ",5)){
                if(pp_depth>0){
                    int pa = pp_depth>1?((pp_stack[pp_depth-2]>>1)&1):1;
                    int taken = pp_stack[pp_depth-1]&1;
                    int en = pa&&!taken&&pp_eval(p+5,defs,*ndefs);
                    pp_stack[pp_depth-1]=(en<<1)|(taken||en);
                }
            } else if(!strncmp(p,"else",4)&&(p[4]=='\0'||p[4]==' ')){
                if(pp_depth>0){
                    int pa = pp_depth>1?((pp_stack[pp_depth-2]>>1)&1):1;
                    int taken = pp_stack[pp_depth-1]&1;
                    int en = pa&&!taken;
                    pp_stack[pp_depth-1]=(en<<1)|(taken||en);
                }
            } else if(!strncmp(p,"endif",5)){
                if(pp_depth>0) pp_depth--;
            } else if(active&&!strncmp(p,"include ",8)){
                char *q=p+8; while(*q==' '||*q=='\t')q++;
                if(*q=='"'){
                    q++; char *end=strchr(q,'"');
                    if(end){
                        *end='\0'; char *want=q;
                        char *res=resolve_include(dir,want);
                        if(!res){ fprintf(stderr,"binc: warning: cannot find include file \"%s\"\n",want); }
                        else {
                            int cyc=0; for(int i=0;i<ninc_stack;i++) if(!strcmp(inc_stack[i],res)){ cyc=1; break; }
                            if(!cyc&&!is_once(res)){
                                FILE *f=fopen(res,"rb");
                                if(f){
                                    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
                                    char *txt=malloc((size_t)n+1); if(n)fread(txt,1,n,f); txt[n]='\0'; fclose(f);
                                    if(n>=3&&(unsigned char)txt[0]==0xEF&&(unsigned char)txt[1]==0xBB&&(unsigned char)txt[2]==0xBF) memmove(txt,txt+3,(size_t)n-2);
                                    if(strstr(txt,"#pragma once")) mark_once(res);
                                    inc_stack[ninc_stack++]=res;
                                    pp_process(res,txt,out,defs,ndefs);
                                    ninc_stack--;
                                    free(txt);
                                }
                            }
                            free(res);
                        }
                    }
                }
            } else if(active&&!strncmp(p,"define ",7)){
                char *nm=p+7; char *sp=nm;
                while(*sp&&*sp!=' '&&*sp!='\t'&&*sp!='(') sp++;
                char *val=sp; while(*val==' '||*val=='\t') val++;
                if(sp>nm&&*ndefs<16384){
                    char params[8][64]; int nparams=0; int fl=0;
                    if(*sp=='('){ /* function-like macro: NAME(a, b) body */
                        fl=1;
                        char *q2=sp+1;
                        while(*q2&&*q2!=')'){
                            while(*q2==' '||*q2=='\t'||*q2==',') q2++;
                            if(*q2==')') break;
                            char *pe=q2; while(*pe&&*pe!=' '&&*pe!='\t'&&*pe!=','&&*pe!=')') pe++;
                            if(nparams<8&&(size_t)(pe-q2)<64){ memcpy(params[nparams],q2,(size_t)(pe-q2)); params[nparams][pe-q2]=0; nparams++; }
                            q2=pe;
                        }
                        char *close=strchr(sp,')'); val=close?close+1:val;
                        while(*val==' '||*val=='\t') val++;
                    }
                    *sp=0;
                    char *ve=val+strlen(val);
                    while(ve>val&&(ve[-1]==' '||ve[-1]=='\t'||ve[-1]=='\r')) *--ve=0;
                    char *cm=val; while((cm=strstr(cm,"//"))){ *cm=0; break; }
                    /* replace a same-name define (UE redefines per config) */
                    Def *old=(Def*)pp_find(defs,*ndefs,nm);
                    if(old){ free(old->name); free(old->val); old->name=strdup(nm); old->val=strdup(val); old->nparams=nparams;
                        for(int pi=0;pi<nparams;pi++) memcpy(old->params[pi],params[pi],64); }
                    else { defs[*ndefs].name=strdup(nm); defs[*ndefs].val=strdup(val); defs[*ndefs].nparams=nparams;
                        for(int pi=0;pi<nparams;pi++) memcpy(defs[*ndefs].params[pi],params[pi],64);
                        (*ndefs)++; }
                    if(getenv("BINC_DUMP_PP")&&fl) fprintf(stderr,"DBG col: %s np=%d\n",nm,nparams);
                    (void)fl;
                }
            } else if(active&&!strncmp(p,"error ",6)){
                fprintf(stderr,"binc: warning: #error %s\n",p+6);
            } else if(active&&!strncmp(p,"undef ",6)){
                char *nm=p+6; while(*nm==' '||*nm=='\t')nm++;
                char *e=nm; while(*e&&*e!=' '&&*e!='\t') e++; *e=0;
                for(size_t di=0;di<*ndefs;di++) if(!strcmp(defs[di].name,nm)){
                    free(defs[di].name); free(defs[di].val);
                    defs[di]=defs[*ndefs-1]; (*ndefs)--;
                    break;
                }
            }
            free(ln);
        } else if(active){
            /* substitute with the live defs table (position-correct scoping:
             * `#define FDFType FDFVector3 ... #undef FDFType` instantiations) */
            if(strstr(L,"%{")){ /* SCW generator placeholder (%{name}): the
                worker fills these at generation time. Substitute a live define
                of that name (the stubs define e.g. pixel_material_inputs), or
                drop the line. */
                const char *pb=strstr(L,"%{");
                const char *pe=pb?strchr(pb+2,'}'):NULL;
                if(pb&&pe){
                    char pname[64]; size_t pl=(size_t)(pe-pb-2);
                    if(pl>=sizeof pname) pl=sizeof pname-1;
                    memcpy(pname,pb+2,pl); pname[pl]=0;
                    const Def *pd=pp_find(defs,*ndefs,pname);
                    if(pd){ bput(out,pd->val,strlen(pd->val)); bput(out,"\n",1); }
                }
            }
            else if(strstr(L,"_Pragma(")){ /* DXC diagnostic directives
                (_Pragma("dxc diagnostic ...")) — meaningless outside DXC; drop */ }
            else {
            if(getenv("BINC_DUMP_PP")&&strstr(L,"DEFINE_ATMOSPHERELIGHTVECTOR")){
                fprintf(stderr,"DBG sub: line='%.60s' ndefs=%zu\n",L,*ndefs);
                const Def *df=pp_find(defs,*ndefs,"DEFINE_ATMOSPHERELIGHTVECTOR");
                fprintf(stderr,"DBG atmo: found=%s np=%d val='%.50s'\n",df?"yes":"NO",df?df->nparams:-1,df?df->val:"");
            }
            if(getenv("BINC_DUMP_PP")&&(strstr(L,"CALL_SITE_DEBUGLOC")||strstr(L,"OPTIONAL_IsFrontFace"))){
                fprintf(stderr,"DBG sub: line='%s' ndefs=%zu\n",L,*ndefs);
                for(size_t di=0;di<*ndefs;di++) if(strstr(defs[di].name,"FrontFace")) fprintf(stderr,"DBG def: '%s' val='%.40s' np=%d\n",defs[di].name,defs[di].val,defs[di].nparams);
            }
            char *sub=pp_subst_line(L,defs,*ndefs);
            if(getenv("BINC_DUMP_PP")&&strstr(L,"CALL_SITE_DEBUGLOC")) fprintf(stderr,"DBG sub -> '%s'\n",sub?sub:"(null)");
            if(sub&&strstr(sub,"_Pragma(")){ /* SHADER_PUSH/POP_WARNINGS_STATE
                expand to _Pragma("dxc diagnostic ...") — drop the expansion */ free(sub); }
            else { bput(out,sub?sub:L,strlen(sub?sub:L)); free(sub);
            bput(out,"\n",1); }
            }
        }
    }
    free(dir);
}

static char *find_prelude(const char *argv0){
    const char *slash=strrchr(argv0,'/');
    if(slash){ char d[512]; size_t l=(size_t)(slash-argv0); if(l>sizeof d-1)l=sizeof d-1;
        memcpy(d,argv0,l); d[l]='\0';
        char p[600]; snprintf(p,sizeof p,"%s/prelude.binc",d);
        if(!access(p,R_OK)) return strdup(p); }
    for(size_t i=0;i<ninc_dirs;i++){ char p[600]; snprintf(p,sizeof p,"%s/prelude.binc",inc_dirs[i]);
        if(!access(p,R_OK)) return strdup(p); }
    if(!access("prelude.binc",R_OK)) return strdup("prelude.binc");
    return NULL;
}

static char *read_file(const char *path, size_t *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) die(0, "cannot open %s", path);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1); fread(buf, 1, n, f); buf[n] = '\0'; fclose(f);
    if (out_n) *out_n = (size_t)n;
    return buf;
}

static void base_name(const char *path, char *out, size_t n) {
    const char *slash = strrchr(path, '/'); const char *b = slash ? slash + 1 : path;
    snprintf(out, n, "%s", b);
    char *dot = strrchr(out, '.'); if (dot) *dot = '\0';
}
static const char *host_type(TypeKind k){
    switch(k){ case T_FLOAT:return "float"; case T_HALF:return "float"; case T_INT32:return "int32_t";
        case T_UINT32:return "uint32_t"; case T_BOOL:return "bool"; default:return "uint32_t"; }
}
static void emit_host_header(const char *lib, const Program *p){
    char hp[800]; snprintf(hp,sizeof hp,"%s",lib); char *dot=strrchr(hp,'.'); if(dot)strcpy(dot,".h"); else strncat(hp,".h",sizeof hp-strlen(hp)-1);
    FILE *h=fopen(hp,"wb"); if(!h)die(0,"cannot write host header %s",hp);
    char guard[128]; base_name(hp,guard,sizeof guard); for(char *q=guard;*q;q++) if(*q<'0'||(*q>'9'&&*q<'A')||(*q>'Z'&&*q<'a')||*q>'z')*q='_';
    fprintf(h,"#ifndef BINC_%s_H\n#define BINC_%s_H\n#include <stdint.h>\n#include <stddef.h>\n#include \"binc_runtime.h\"\n\n",guard,guard);
    for(size_t fi=0;fi<p->nfuncs;fi++){ Function *f=&p->funcs[fi]; if(!f->is_kernel)continue;
        fprintf(h,"static inline int binc_%s(BincRuntime *rt, size_t grid",f->name);
        for(size_t pi=0;pi<f->nparams;pi++){ Param *x=&f->params[pi]; if(x->ty.array_n||x->ty.kind==T_COORD||x->ty.kind==T_GRID_EXTENT)continue;
            if(x->ty.is_ptr) fprintf(h,", BincBuffer *%s",x->name); else { char tn[64]; if(x->ty.vecn>1) snprintf(tn,sizeof tn,"/* vector */ uint32_t"); else snprintf(tn,sizeof tn,"%s",host_type(x->ty.kind)); fprintf(h,", %s %s",tn,x->name); } }
        fprintf(h,"){ BincDispatchArg a[%d]; int n=0;\n",(int)f->nparams+1);
        for(size_t pi=0;pi<f->nparams;pi++){ Param *x=&f->params[pi]; if(x->ty.array_n||x->ty.kind==T_COORD||x->ty.kind==T_GRID_EXTENT)continue;
            if(x->ty.is_ptr) fprintf(h,"    a[n++]=binc_arg_buffer(%d, %s);\n",(int)pi,x->name);
            else fprintf(h,"    a[n++]=binc_arg_bytes(%d, &%s, sizeof(%s));\n",(int)pi,x->name,x->name); }
        fprintf(h,"    return binc_runtime_dispatch(rt, \"%s\", grid, a, n); }\n\n",f->name);
    }
    fprintf(h,"#endif\n"); fclose(h); fprintf(stderr,"binc: generated host bindings -> %s\n",hp);
}

int main(int argc, char **argv) {
    /* Detect the installed Metal toolchain's SDK and derive the AIR contract:
     * air64_v<SDK+2>-apple-macosx<SDK>.0.0 with !air.version 2.<SDK-18>.
     * The local Xcode beta (SDK 27) emits v29/2.9; GitHub CI's stable Xcode
     * (SDK 26) expects v28/2.8 — hardcoding either breaks the other. */
    {
        int sdk=27;
        FILE *pf=popen("xcrun --show-sdk-version 2>/dev/null","r");
        if(pf){ char buf[64]; if(fgets(buf,sizeof buf,pf)) sdk=atoi(buf); pclose(pf); }
        if(sdk<20) sdk=27; /* probe failed: keep the known-good default */
        char triple[64]; snprintf(triple,sizeof triple,"air64_v%d-apple-macosx%d.0.0",sdk+2,sdk);
        binc_set_air(triple,sdk,sdk-18);
    }
    const char *infile = NULL; const char *outfile = NULL; int emit_ll_only = 0; int no_prelude = 0; int interpret = 0; int syntax_only = 0; int stage_all = 0; int reflect = 0;
    const char *hlsl_entry = NULL; const char *hlsl_profile = NULL;
    char *ddefs[32]; size_t nddefs=0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i+1 < argc) outfile = argv[++i];
        else if (!strcmp(argv[i], "-I") && i+1 < argc) add_inc_dir(argv[++i]);
        else if (!strcmp(argv[i], "-D") && i+1 < argc && nddefs < 32) ddefs[nddefs++] = argv[++i];
        else if (!strcmp(argv[i], "--emit-ll")) emit_ll_only = 1;
        else if (!strcmp(argv[i], "-no-prelude")) no_prelude = 1;
        else if (!strcmp(argv[i], "-i")) interpret = 1;
        else if (!strcmp(argv[i], "-fsyntax-only")) syntax_only = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { fputs(usage_text, stdout); return 0; }
        else if (!strcmp(argv[i], "--version")) {
            /* prefer the VERSION file (next to the binary, then cwd) */
            char vpath[640]={0}; int have=0;
            const char *slash=strrchr(argv[0],'/');
            if(slash){ size_t l=(size_t)(slash-argv[0]); if(l>500)l=500;
                snprintf(vpath,sizeof vpath,"%.*s/VERSION",(int)l,argv[0]);
                if(!access(vpath,R_OK)) have=1; }
            if(!have && !access("VERSION",R_OK)){ snprintf(vpath,sizeof vpath,"VERSION"); have=1; }
            if(have){ FILE *vf=fopen(vpath,"r"); char vb[64]={0};
                if(vf){ if(fgets(vb,sizeof vb,vf)){ char *nl=strchr(vb,'\n'); if(nl)*nl='\0'; } fclose(vf); }
                if(vb[0]){ printf("binc %s\n",vb); return 0; } }
            printf("binc %s\n", BINC_VERSION); return 0;
        }
        else if (!strcmp(argv[i], "-E") && i + 1 < argc) { hlsl_entry = argv[++i]; }
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) { hlsl_profile = argv[++i]; }
        else if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--stage-all")) { stage_all = 1; }
        else if (!strcmp(argv[i], "--reflect")) { reflect = 1; }
        else if (argv[i][0] != '-') infile = argv[i];
        else { fprintf(stderr, "binc: unknown option %s\n", argv[i]); fputs(usage_text, stderr); return 2; }
    }
    if (!infile) { fputs(usage_text, stderr); return 2; }

    /* ---- HLSL frontend routing (Phases 1-7 of the HLSL-to-Metal plan) ----
     * `binc -E <entry> -T <profile> file.hlsl` mirrors DXC's interface. */
    const char *entry = hlsl_entry ? hlsl_entry : "main";
    const char *profile = hlsl_profile;
    const char *ext = strrchr(infile, '.');
    int is_hlsl = ext && (!strcmp(ext, ".hlsl") || !strcmp(ext, ".fx") || !strcmp(ext, ".fxh")
                          || !strcmp(ext, ".usf") || !strcmp(ext, ".ush"));
    if (is_hlsl && !profile) {
        fprintf(stderr, "binc: %s looks like an HLSL shader — compile with -T <profile> (e.g. -T vs_5_0)\n", infile);
        return 3;
    }
    if (is_hlsl) {
        /* validate the target profile: vs_5_0 / ps_5_0 / cs_5_0 / gs_4_0 / *_3_0 ... */
        int maj = -1, min = -1; char stage[4] = {0};
        if (sscanf(profile, "%3[vspscsgs]_%d_%d", stage, &maj, &min) != 3 ||
            (strcmp(stage, "vs") && strcmp(stage, "ps") && strcmp(stage, "cs") && strcmp(stage, "gs")))
            { fprintf(stderr, "binc: invalid target profile '%s' (expected vs/ps/cs/gs_N_M)\n", profile); return 2; }
    }

    /* preprocess: for HLSL, raw file with #-directives stripped (no prelude);
     * otherwise the optional prelude, then the user file, splicing includes */
    char *src; int first_line = 1;
    if (is_hlsl) {
        char *main_spl=splice_file(infile);
        if(!main_spl) return 1;
        /* HLSL preprocessor: #if/#ifdef/#elif/#else/#endif evaluated over the
         * collected defines; #include spliced recursively (active branches
         * only); #define NAME value collected for whole-word substitution. */
        Buf all={0};
        Def *defs=calloc(16384,sizeof(Def)); size_t ndefs=0; /* heap: 16K x ~540B > the 8MB stack */
        /* -D NAME[=value] command-line defines (UE's ShaderCompileWorker passes
         * dozens: COMPILER_DXC, PLATFORM_*, ENGINE_*, ...) */
        for(size_t dd=0;dd<nddefs&&ndefs<16384;dd++){
            const char *dv=ddefs[dd]; const char *eq=strchr(dv,'=');
            char nm[256]; size_t nl=eq?(size_t)(eq-dv):strlen(dv);
            if(nl>=sizeof nm) nl=sizeof nm-1;
            memcpy(nm,dv,nl); nm[nl]=0;
            defs[ndefs].name=strdup(nm); defs[ndefs].val=strdup(eq?eq+1:"1"); defs[ndefs].nparams=0;
            ndefs++;
        }
        pp_process(infile,main_spl,&all,defs,&ndefs);
        free(main_spl);
        if(getenv("BINC_DUMP_PP")){ FILE *pp0=fopen("/tmp/binc_pp0.txt","wb"); fwrite(all.p,1,strlen(all.p),pp0); fclose(pp0); }
        src=all.p;
        if(getenv("BINC_DUMP_PP")){ FILE *pp=fopen("/tmp/binc_pp.txt","wb"); fwrite(src,1,strlen(src),pp); fclose(pp);
            for(size_t d=0;d<ndefs;d++){ fprintf(stderr,"DBG def: %s np=%d p0=%s val=%.30s\n",defs[d].name,defs[d].nparams,defs[d].nparams?defs[d].params[0]:"-",defs[d].val); } }
        free(defs);
    } else {
    {
        Buf all={0};
        if(!no_prelude){
            char *pre=find_prelude(argv[0]);
            if(pre){ char *spl=splice_file(pre);
                if(!spl){ fprintf(stderr,"binc: error: prelude %s failed to load\n",pre); return 1; }
                /* count prelude lines so user-file diagnostics keep their true line numbers */
                for(const char *q=spl;*q;q++) if(*q=='\n') first_line++;
                first_line = 1 - first_line; /* plus the separator newline the driver adds */
                bput(&all,spl,strlen(spl)); bput(&all,"\n",1); free(spl); free(pre); }
        }
        char *main_spl=splice_file(infile);
        if(!main_spl) return 1;
        bput(&all,main_spl,strlen(main_spl)); free(main_spl);
        src=all.p;
    }
    }

    Token *toks; size_t ntoks;
    lex(src, &toks, &ntoks, first_line, is_hlsl);
    TokStream ts = { toks, ntoks, 0 };
    Program prog;
    if (is_hlsl) {
        HLSLProg hprog = hlsl_parse(&ts);
        if (had_errors()) return 1;
        if (syntax_only) return 0; /* HLSL parse acceptance (lit conformance gate) */
        prog = hlsl_build(&hprog, entry, profile, stage_all);
        if (reflect) { binc_reflect(stdout, &hprog, &prog); return 0; }
    }
    else prog = parse_program(&ts);
    if (had_errors()) return 1;
    if (interpret) { interp_run(&prog); return 0; }
    if (syntax_only) {
        /* the codegen is the type checker: run it into the void and report */
        FILE *nullout = fopen("/dev/null", "wb");
        if (!nullout) { fprintf(stderr, "binc: cannot open /dev/null\n"); return 1; }
        emit_air(nullout, &prog);
        fclose(nullout);
        return had_errors() ? 1 : 0;
    }

    char base[512]; base_name(infile, base, sizeof base);
    char ll[600], air[600], lib[700];
    snprintf(ll,  sizeof ll,  "%s.ll", base);
    snprintf(air, sizeof air, "%s.air", base);
    snprintf(lib, sizeof lib, "%s", outfile ? outfile : (snprintf(lib,sizeof lib,"%s.metallib",base), lib));

    FILE *out = fopen(ll, "wb");
    if (!out) die(0, "cannot write %s", ll);
    emit_air(out, &prog);
    fclose(out);
    if (had_errors()) { remove(ll); return 1; }
    fprintf(stderr, "binc: emitted AIR -> %s\n", ll);

    if (emit_ll_only) { fprintf(stderr, "binc: stopped after AIR emission (--emit-ll)\n"); return 0; }

    char cmd[1600];
    snprintf(cmd, sizeof cmd, "%s %s -c -o %s 2>&1", tool("METAL","xcrun metal"), ll, air);
    fprintf(stderr, "binc: $ %s\n", cmd);
    int rc = system(cmd);
    if (rc != 0) { fprintf(stderr, "binc: metal front-end failed (exit %d)\n", rc); return 1; }

    snprintf(cmd, sizeof cmd, "%s %s -o %s 2>&1", tool("METALLIB","xcrun metallib"), air, lib);
    fprintf(stderr, "binc: $ %s\n", cmd);
    rc = system(cmd);
    if (rc != 0) { fprintf(stderr, "binc: metallib link failed (exit %d)\n", rc); return 1; }

    emit_host_header(lib, &prog);
    fprintf(stderr, "binc: ✓ %s -> %s\n", infile, lib);
    return 0;
}
