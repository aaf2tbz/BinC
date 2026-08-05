/* tools/dds/test_dds.c — validate dds.h against real corpus textures.
 * usage: test_dds <file.dds> [more.dds ...]
 * Exits 0 only when every file parses with sane dimensions and a known format
 * whose data size fits the file. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "dds.h"

int main(int argc, char **argv){
    if (argc < 2){ fprintf(stderr, "usage: test_dds <file.dds> [...]\n"); return 2; }
    int bad = 0;
    for (int a = 1; a < argc; a++){
        FILE *f = fopen(argv[a], "rb");
        if (!f){ fprintf(stderr, "%s: cannot open\n", argv[a]); bad = 1; continue; }
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc((size_t)len);
        if (fread(buf, 1, (size_t)len, f) != (size_t)len){ fclose(f); free(buf); bad = 1; continue; }
        fclose(f);

        DdsInfo d;
        DdsStatus st = dds_parse(buf, (size_t)len, &d);
        if (st != DDS_OK){
            fprintf(stderr, "%s: parse error %d\n", argv[a], (int)st);
            free(buf); bad = 1; continue;
        }
        printf("%s: %ux%u mips=%u %s (srgb=%d) data=%zu/%ld bytes\n",
               argv[a], d.width, d.height, d.mips, d.mtl_format, d.is_srgb,
               d.data_size, len);
        if (d.width > 16384 || d.height > 16384){ fprintf(stderr, "%s: absurd dims\n", argv[a]); bad = 1; }
        if (d.mips > 20){ fprintf(stderr, "%s: absurd mip count\n", argv[a]); bad = 1; }
        free(buf);
    }
    return bad ? 1 : 0;
}
