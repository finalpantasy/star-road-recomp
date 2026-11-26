/* Star Road Recomp task-24 helper: patch one save image with the unlock_postgame code path and write the result.
 * usage: postgame_save_tool <in> <out> [off]   (off: calls the gate with enabled=0, which must return NULL and leave the file alone)
 * Exit 0 ok, 1 error, 2 not recognized as a save, 3 off mode and the gate returned NULL (the expected result). Our own code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/mods/postgame.h"

static void logcb(void *ud, const char *line) { (void)ud; printf("%s\n", line); }

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *in = malloc((size_t)n);
    if (!in || fread(in, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    size_t on = 0;
    int off = argc > 3 && !strcmp(argv[3], "off");
    uint8_t *out = postgame_filter_save(off ? 0 : 1, in, (size_t)n, &on, logcb, 0);
    if (!out) return off ? 3 : 2;
    f = fopen(argv[2], "wb");
    if (!f || fwrite(out, 1, on, f) != on) return 1;
    fclose(f);
    free(out);
    free(in);
    return 0;
}
