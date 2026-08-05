/* main.c — binc driver: read .binc, lex, parse, emit AIR .ll, drive metal/metallib -> .metallib */
#include "binc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define BINC_VERSION "0.1.0"

static const char *usage_text =
    "usage: binc <file.binc> [options]\n"
    "options:\n"
    "  -o <file.metallib>   output metallib path (default: <base>.metallib)\n"
    "  --emit-ll            stop after emitting AIR .ll (no metal/metallib)\n"
    "  -h, --help           show this help and exit\n"
    "  --version            show the compiler version and exit\n"
    "environment: METAL / METALLIB override the AIR tool invocations\n"
    "             (defaults: \"xcrun metal\" / \"xcrun metallib\")\n";

/* METAL/METALLIB env override, defaulting to xcrun (works on local and CI
 * macOS runners regardless of which Xcode is selected). */
static const char *tool(const char *env, const char *def){
    const char *v = getenv(env);
    return v && *v ? v : def;
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
    const char *infile = NULL; const char *outfile = NULL; int emit_ll_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i+1 < argc) outfile = argv[++i];
        else if (!strcmp(argv[i], "--emit-ll")) emit_ll_only = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { fputs(usage_text, stdout); return 0; }
        else if (!strcmp(argv[i], "--version")) { printf("binc %s\n", BINC_VERSION); return 0; }
        else if (argv[i][0] != '-') infile = argv[i];
        else { fprintf(stderr, "binc: unknown option %s\n", argv[i]); fputs(usage_text, stderr); return 2; }
    }
    if (!infile) { fputs(usage_text, stderr); return 2; }

    size_t srclen; char *src = read_file(infile, &srclen);

    Token *toks; size_t ntoks;
    lex(src, &toks, &ntoks);
    TokStream ts = { toks, ntoks, 0 };
    Program prog = parse_program(&ts);
    if (had_errors()) return 1;

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
