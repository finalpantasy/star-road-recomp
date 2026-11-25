/* Star Road Recomp options store tests (task-16). Our own code.
 * usage: test_options OUTDIR ROOT        runs the checks (OUTDIR is scratch space, ROOT the repo root)
 *        test_options --emit FILE        writes the default ini text (how config/options.default.ini is produced) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../../runtime/platform/overlay/options.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); g_checks++; if (_a != _b) { g_fail++; printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); g_checks++; if (strcmp(_a, _b)) { g_fail++; printf("FAIL %s:%d: %s == \"%s\" (got \"%s\")\n", __FILE__, __LINE__, #a, _b, _a); } } while (0)

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = (char *)calloc(1, 1 << 16);
    *n = fread(b, 1, (1 << 16) - 1, f);
    fclose(f);
    return b;
}

static void spit(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void test_defaults_and_table(void) {
    Options o;
    opts_defaults(&o);
    CHECK_EQ(OPT_COUNT, 9);
    for (int i = 0; i < OPT_COUNT; i++) {
        const OptDesc *d = opt_desc(i);
        CHECK(d && d->key && d->section && d->page && d->label && d->help);
        CHECK(opt_is_default(&o, i));
        CHECK(!opt_needs_restart(&o, i));
        if (d->type == OPT_ENUM) { CHECK(d->n_names >= 2); CHECK(d->def >= 0 && d->def < d->n_names); CHECK_EQ(d->max, d->n_names - 1); }
    }
    /* the faithful defaults (D-004): nothing changes the picture, the pacing or the sound until the player asks */
    CHECK_EQ(opt_get(&o, OPT_UPSCALE), 0);           /* off */
    CHECK_EQ(opt_get(&o, OPT_UPSCALE_TARGET), 0);    /* window */
    CHECK_EQ(opt_get(&o, OPT_FPS_CAP), 0);           /* game */
    CHECK_EQ(opt_get(&o, OPT_AUDIO), 0);             /* muted: audio stays off until asked for */
    CHECK_EQ(opt_get(&o, OPT_VOLUME), 100);
    CHECK_EQ(opt_get(&o, OPT_WINDOW_MODE), 0);
    CHECK_STR(opt_get_path(&o, OPT_INPUT_MAP), "");
    CHECK_EQ(opt_get(&o, OPT_UNLOCK_POSTGAME), 0);   /* off: the faithful game */
    opts_free(&o);
}

static void test_default_file(const char *root) {
    char path[512];
    snprintf(path, sizeof path, "%s\\config\\options.default.ini", root);
    size_t n = 0;
    char *file = slurp(path, &n);
    CHECK(file != NULL);
    if (!file) return;
    char *want = (char *)calloc(1, 1 << 16);
    opts_default_ini(want, 1 << 16);
    /* ignore CRLF versus LF (git may convert the checked-out copy) */
    char *a = (char *)calloc(1, n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) if (file[i] != '\r') a[k++] = file[i];
    CHECK_STR(a, want);
    /* the default file, read back, is the defaults and marks nothing as changed */
    Options o;
    int got = opts_load(&o, path);
    CHECK_EQ(got, OPT_COUNT);
    for (int i = 0; i < OPT_COUNT; i++) CHECK(opt_is_default(&o, i));
    opts_free(&o);
    free(file); free(want); free(a);
}

static void test_parse(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s\\parse.ini", dir);
    spit(path,
         "; comment\n# another\n\n"
         "[display]\n"
         "upscale = FSR2   ; names are case-insensitive, trailing comments are dropped\n"
         "upscale_target = 2160p\n"
         "sharpness = bogus\n"                      /* bad value: skipped, default stays */
         "window_mode=fullscreen\n"
         "[performance]\n"
         "fps_cap = 144\n"
         "[audio]\n"
         "enabled = yes\n"
         "volume = 250\n"                           /* clamped to 100 */
         "[controls]\n"
         "input_map = C:\\games\\my keys.ini\n"
         "[gameplay]\n"
         "unlock_postgame = on\n"
         "[future]\n"
         "flux = 9\n"                               /* unknown: kept */
         "garbage line without equals\n");
    Options o;
    int n = opts_load(&o, path);
    CHECK_EQ(n, 8);                                  /* 9 known keys in the file, the bad sharpness line is skipped */
    CHECK_EQ(opt_get(&o, OPT_UPSCALE), 2);
    CHECK_EQ(opt_get(&o, OPT_UPSCALE_TARGET), 3);
    CHECK_EQ(opt_get(&o, OPT_SHARPNESS), 3);        /* default "default" */
    CHECK_EQ(opt_get(&o, OPT_WINDOW_MODE), 1);
    CHECK_EQ(opt_get(&o, OPT_FPS_CAP), 5);           /* 144 */
    CHECK_EQ(opt_get(&o, OPT_AUDIO), 1);
    CHECK_EQ(opt_get(&o, OPT_VOLUME), 100);
    CHECK_STR(opt_get_path(&o, OPT_INPUT_MAP), "C:\\games\\my keys.ini");
    CHECK(o.extra && strstr(o.extra, "[future]\nflux = 9\n"));
    CHECK_EQ(opt_get(&o, OPT_UNLOCK_POSTGAME), 1);
    CHECK(!opt_needs_restart(&o, OPT_UNLOCK_POSTGAME));   /* not a restart option: the game reads it at the next load */
    CHECK(!opt_needs_restart(&o, OPT_AUDIO));       /* what the file said at start is what the game started with */
    char t[64];
    opt_text(&o, OPT_UPSCALE, t, sizeof t); CHECK_STR(t, "fsr2");
    opt_text(&o, OPT_FPS_CAP, t, sizeof t); CHECK_STR(t, "144");
    opt_text(&o, OPT_VOLUME, t, sizeof t); CHECK_STR(t, "100");
    opt_text(&o, OPT_INPUT_MAP, t, sizeof t); CHECK_STR(t, "my keys.ini");
    opts_free(&o);
    CHECK_EQ(opts_load(&o, "A:\\does\\not\\exist.ini"), -1);
}

static void test_set_step(void) {
    Options o;
    opts_defaults(&o);
    unsigned rev = o.revision;
    CHECK_EQ(opt_set(&o, OPT_UPSCALE, 1), 1);
    CHECK(o.revision != rev);
    CHECK_EQ(opt_set(&o, OPT_UPSCALE, 1), 0);        /* no change, no revision bump */
    CHECK_EQ(opt_set(&o, OPT_UPSCALE, 99), 1);       /* clamped to the last name */
    CHECK_EQ(opt_get(&o, OPT_UPSCALE), 2);
    CHECK_EQ(opt_set(&o, OPT_UPSCALE, -4), 1);
    CHECK_EQ(opt_get(&o, OPT_UPSCALE), 0);
    /* enums and bools wrap */
    CHECK_EQ(opt_step(&o, OPT_UPSCALE, -1), 1);
    CHECK_EQ(opt_get(&o, OPT_UPSCALE), 2);
    CHECK_EQ(opt_step(&o, OPT_UPSCALE, +1), 1);
    CHECK_EQ(opt_get(&o, OPT_UPSCALE), 0);
    CHECK_EQ(opt_step(&o, OPT_AUDIO, +1), 1);
    CHECK_EQ(opt_get(&o, OPT_AUDIO), 1);
    CHECK_EQ(opt_step(&o, OPT_AUDIO, +1), 1);
    CHECK_EQ(opt_get(&o, OPT_AUDIO), 0);
    /* ints step and clamp */
    CHECK_EQ(opt_step(&o, OPT_VOLUME, +1), 0);       /* already 100 */
    CHECK_EQ(opt_step(&o, OPT_VOLUME, -1), 1);
    CHECK_EQ(opt_get(&o, OPT_VOLUME), 95);
    for (int i = 0; i < 40; i++) opt_step(&o, OPT_VOLUME, -1);
    CHECK_EQ(opt_get(&o, OPT_VOLUME), 0);
    CHECK_EQ(opt_step(&o, OPT_VOLUME, -1), 0);
    /* path */
    CHECK_EQ(opt_set_path(&o, OPT_INPUT_MAP, "x.ini"), 1);
    CHECK_EQ(opt_set_path(&o, OPT_INPUT_MAP, "x.ini"), 0);
    CHECK(!opt_is_default(&o, OPT_INPUT_MAP));
    opt_reset(&o, OPT_INPUT_MAP);
    CHECK(opt_is_default(&o, OPT_INPUT_MAP));
    CHECK_EQ(opt_set_path(&o, OPT_UPSCALE, "no"), 0);   /* not a path option */
    /* reset */
    opt_reset(&o, OPT_VOLUME);
    CHECK_EQ(opt_get(&o, OPT_VOLUME), 100);
    opts_free(&o);
}

static void test_restart_tracking(void) {
    Options o;
    opts_defaults(&o);
    CHECK(!opts_any_restart(&o));
    opt_set(&o, OPT_VOLUME, 50);                     /* live option: never "on restart" */
    CHECK(!opt_needs_restart(&o, OPT_VOLUME));
    CHECK(!opts_any_restart(&o));
    opt_set(&o, OPT_AUDIO, 1);                       /* restart option changed since the game started */
    CHECK(opt_needs_restart(&o, OPT_AUDIO));
    CHECK(opts_any_restart(&o));
    opt_set(&o, OPT_AUDIO, 0);                       /* back to what the game started with */
    CHECK(!opts_any_restart(&o));
    opts_free(&o);
}

static void test_save_roundtrip(const char *dir) {
    char path[512], tmp[520], seed[512];
    snprintf(path, sizeof path, "%s\\rt.ini", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    snprintf(seed, sizeof seed, "%s\\seed.ini", dir);
    spit(seed, "[future]\nflux = 9\n[audio]\nvolume = 35\n");
    Options o;
    CHECK_EQ(opts_load(&o, seed), 1);
    CHECK_EQ(opt_get(&o, OPT_VOLUME), 35);
    opt_set(&o, OPT_UPSCALE, 2);
    opt_set(&o, OPT_UPSCALE_TARGET, 2);
    opt_set(&o, OPT_FPS_CAP, 3);
    opt_set(&o, OPT_WINDOW_MODE, 1);
    opt_set(&o, OPT_AUDIO, 1);
    opt_set(&o, OPT_SHARPNESS, 0);
    opt_set_path(&o, OPT_INPUT_MAP, "C:\\k\\input_alt.ini");
    CHECK_EQ(opts_save(&o, path), 0);
    CHECK(GetFileAttributesA(tmp) == INVALID_FILE_ATTRIBUTES);     /* the temp file is gone after the rename */
    Options r;
    CHECK_EQ(opts_load(&r, path), OPT_COUNT);
    for (int i = 0; i < OPT_COUNT; i++) {
        CHECK_EQ(opt_get(&r, i), opt_get(&o, i));
        CHECK_STR(opt_get_path(&r, i), opt_get_path(&o, i));
    }
    CHECK(r.extra && strstr(r.extra, "[future]\nflux = 9\n"));    /* unknown key survived the load, the save and the load */
    /* saving twice is stable */
    char path2[512];
    snprintf(path2, sizeof path2, "%s\\rt2.ini", dir);
    CHECK_EQ(opts_save(&r, path2), 0);
    size_t n1 = 0, n2 = 0;
    char *a = slurp(path, &n1), *b = slurp(path2, &n2);
    CHECK(a && b && n1 == n2 && !memcmp(a, b, n1));
    free(a); free(b);
    /* saving over an existing file replaces it */
    opt_set(&o, OPT_UPSCALE, 0);
    CHECK_EQ(opts_save(&o, path), 0);
    Options r2;
    CHECK(opts_load(&r2, path) == OPT_COUNT && opt_get(&r2, OPT_UPSCALE) == 0);
    /* an unwritable target reports failure and leaves nothing behind */
    char bad[512];
    snprintf(bad, sizeof bad, "%s\\no_such_dir\\x.ini", dir);
    CHECK(opts_save(&o, bad) != 0);
    opts_free(&o); opts_free(&r); opts_free(&r2);
}

static void test_file_path(void) {
    char p[512];
    _putenv("SMRPG_OPTIONS=Z:\\somewhere\\o.ini");
    opts_file_path(p, sizeof p);
    CHECK_STR(p, "Z:\\somewhere\\o.ini");
    _putenv("SMRPG_OPTIONS=");
    opts_file_path(p, sizeof p);
    CHECK(strstr(p, "options.ini") != NULL);
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--emit")) {
        char *buf = (char *)calloc(1, 1 << 16);
        size_t n = opts_default_ini(buf, 1 << 16);
        FILE *f = fopen(argv[2], "wb");
        if (!f) return 2;
        fwrite(buf, 1, n, f);
        fclose(f);
        return 0;
    }
    if (argc < 3) { printf("usage: test_options OUTDIR ROOT | --emit FILE\n"); return 2; }
    test_defaults_and_table();
    test_default_file(argv[2]);
    test_parse(argv[1]);
    test_set_step();
    test_restart_tracking();
    test_save_roundtrip(argv[1]);
    test_file_path();
    printf("options: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
