/* main.c — binc driver: read .binc, lex, parse, emit AIR .ll, drive metal/metallib -> .metallib */
#include "binc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

int main(int argc, char **argv) {
    const char *infile = NULL; const char *outfile = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i+1 < argc) outfile = argv[++i];
        else if (argv[i][0] != '-') infile = argv[i];
    }
    if (!infile) { fprintf(stderr, "usage: binc <file.binc> [-o out.metallib]\n"); return 2; }

    size_t srclen; char *src = read_file(infile, &srclen);

    Token *toks; size_t ntoks;
    lex(src, &toks, &ntoks);
    TokStream ts = { toks, ntoks, 0 };
    Program prog = parse_program(&ts);

    char base[512]; base_name(infile, base, sizeof base);
    char ll[600], air[600], lib[700];
    snprintf(ll,  sizeof ll,  "%s.ll", base);
    snprintf(air, sizeof air, "%s.air", base);
    snprintf(lib, sizeof lib, "%s", outfile ? outfile : (snprintf(lib,sizeof lib,"%s.metallib",base), lib));

    FILE *out = fopen(ll, "wb");
    if (!out) die(0, "cannot write %s", ll);
    emit_air(out, &prog);
    fclose(out);
    fprintf(stderr, "binc: emitted AIR -> %s\n", ll);

    char cmd[1600];
    snprintf(cmd, sizeof cmd, "metal %s -c -o %s 2>&1", ll, air);
    fprintf(stderr, "binc: $ %s\n", cmd);
    int rc = system(cmd);
    if (rc != 0) { fprintf(stderr, "binc: metal front-end failed (exit %d)\n", rc); return 1; }

    snprintf(cmd, sizeof cmd, "metallib %s -o %s 2>&1", air, lib);
    fprintf(stderr, "binc: $ %s\n", cmd);
    rc = system(cmd);
    if (rc != 0) { fprintf(stderr, "binc: metallib link failed (exit %d)\n", rc); return 1; }

    fprintf(stderr, "binc: ✓ %s -> %s\n", infile, lib);
    return 0;
}
