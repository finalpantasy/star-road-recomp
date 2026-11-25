/* Star Road Recomp unlock_postgame tests (task-24). Our own code. The fixtures are synthetic: field names and layout follow SaveData in the
 * IL2CPP dump; no value comes from a real save or from the game.
 * usage: test_postgame SAVEDIR     (SAVEDIR is scratch space for the save copies, under A:\AI\Recomps\smrpg-work\save\) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../../runtime/mods/postgame.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); g_checks++; if (_a != _b) { g_fail++; printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static char g_dir[512];
static int g_logn;
static char g_logs[16][160];
static void logcb(void *ud, const char *line) { (void)ud; if (g_logn < 16) snprintf(g_logs[g_logn], 160, "%s", line); g_logn++; }

/* ---- a synthetic save as UTF-16LE JSON (what JsonUtility.ToJson writes; compact, or pretty with pretty=1) ---- */
typedef struct Buf { uint8_t *p; size_t n, cap; } Buf;
static void put(Buf *b, const char *s) {
    for (; *s; s++) {
        if (b->n + 2 > b->cap) { b->cap = b->cap ? b->cap * 2 : 1 << 16; b->p = (uint8_t *)realloc(b->p, b->cap); }
        b->p[b->n++] = (uint8_t)*s; b->p[b->n++] = 0;
    }
}
static void put_flags(Buf *b, const char *key, int count, unsigned seed, int pretty) {
    char t[64];
    snprintf(t, sizeof t, "%s\"%s\":%s[", pretty ? "\n  " : "", key, pretty ? " " : "");
    put(b, t);
    for (int i = 0; i < count; i++) {
        seed = seed * 1664525u + 1013904223u;
        if (i) put(b, pretty ? ",\n    " : ",");
        put(b, ((seed >> 24) & 3) == 0 ? "true" : "false");
    }
    put(b, pretty ? "\n  ]" : "]");
}
static Buf make_save(int chapter, int gflags, int pretty, int bom, int chapter_after_array) {
    Buf b = {0};
    if (bom) { b.p = (uint8_t *)malloc(1 << 16); b.cap = 1 << 16; b.p[0] = 0xFF; b.p[1] = 0xFE; b.n = 2; }
    char t[256];
    put(&b, "{");
    put(&b, "\"_ref_slot\":0,\"_save_version\":11,\"_player_name\":\"Tester\",\"_difficulty\":0,\"_coin\":1234,\"_party_number\":2,\"_party_order\":[1,2,0,0,0],");
    if (!chapter_after_array) { snprintf(t, sizeof t, "\"_chapter_number\":%d,", chapter); put(&b, t); }
    put(&b, "\"_respawn_pos\":{\"x\":1.5,\"y\":-2.25,\"z\":0},\"_map_name\":\"unit_test_map\",\"_play_time\":98765,");
    put(&b, "\"_arasuji_save\":[true,false,true],");
    put_flags(&b, "_global_flag_save", gflags, 7, pretty); put(&b, ",");
    put_flags(&b, "_area_flag_save", 40, 9, pretty); put(&b, ",");
    put_flags(&b, "_local_flag_save", 40, 11, pretty);
    if (chapter_after_array) { snprintf(t, sizeof t, ",\"_chapter_number\":%d", chapter); put(&b, t); }
    put(&b, pretty ? "\n}" : "}");
    return b;
}

/* ---- field-by-field reading: splits the top-level object into key -> raw value text (UTF-16 narrowed to ASCII) ---- */
typedef struct Field { char key[48]; char *val; } Field;
static int isws_(char c) { return c == 32 || c == 10 || c == 13 || c == 9; }
static int split_fields(const Buf *b, Field *f, int max) {
    size_t N = b->n / 2, i = (b->p[0] == 0xFF && b->p[1] == 0xFE) ? 1 : 0;
    char *s = (char *)malloc(N + 1);
    for (size_t k = 0; k < N; k++) s[k] = (char)b->p[k * 2];
    s[N] = 0;
    int nf = 0;
    if (s[i] != '{') { free(s); return 0; }
    i++;
    while (i < N) {
        while (isws_(s[i]) || s[i] == ',') i++;
        if (s[i] != '"') break;                 /* the closing brace */
        size_t ks = ++i;
        while (i < N && s[i] != '"') i++;
        size_t kl = i - ks; if (kl > 47) kl = 47;
        char key[48]; memcpy(key, s + ks, kl); key[kl] = 0;
        i++;
        while (isws_(s[i])) i++;
        if (s[i] != ':') break;
        i++;
        while (isws_(s[i])) i++;
        size_t vs = i;
        int depth = 0, instr = 0;
        for (; i < N; i++) {
            char c = s[i];
            if (instr) { if (c == '"' && s[i - 1] != 0x5c) instr = 0; continue; }
            if (c == '"') { instr = 1; continue; }
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') { if (depth == 0) break; depth--; }
            else if (c == ',' && depth == 0) break;
        }
        size_t ve = i;
        while (ve > vs && isws_(s[ve - 1])) ve--;
        if (nf < max) {
            f[nf].val = (char *)calloc(1, ve - vs + 1);
            memcpy(f[nf].val, s + vs, ve - vs);
            snprintf(f[nf].key, sizeof f[nf].key, "%s", key);
            nf++;
        }
    }
    free(s);
    return nf;
}
static int get_field(const Field *f, int n, const char *k) { for (int i = 0; i < n; i++) if (!strcmp(f[i].key, k)) return i; return -1; }
static void free_fields(Field *f, int n) { for (int i = 0; i < n; i++) free(f[i].val); }

/* the booleans of a "[true,false,...]" value */
static int bools(const char *v, unsigned char *out, int max) {
    int n = 0;
    for (const char *p = v; *p && n < max; p++) {
        if (!strncmp(p, "true", 4)) { out[n++] = 1; p += 3; }
        else if (!strncmp(p, "false", 5)) { out[n++] = 0; p += 4; }
    }
    return n;
}

static void write_file(const char *path, const Buf *b) { FILE *f = fopen(path, "wb"); if (f) { fwrite(b->p, 1, b->n, f); fclose(f); } }
static Buf read_file(const char *path) {
    Buf b = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END); b.n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    b.p = (uint8_t *)malloc(b.n + 2); b.cap = b.n;
    if (fread(b.p, 1, b.n, f) != b.n) b.n = 0;
    fclose(f);
    return b;
}

/* one round trip: save written, patched copy written and read back, every field compared */
static void round_trip(const char *tag, int chapter, int gflags, int pretty, int bom, int chapter_after, int raise) {
    char path_a[600], path_b[600];
    snprintf(path_a, sizeof path_a, "%s\\%s_orig.save", g_dir, tag);
    snprintf(path_b, sizeof path_b, "%s\\%s_on.save", g_dir, tag);
    Buf orig = make_save(chapter, gflags, pretty, bom, chapter_after);
    write_file(path_a, &orig);
    Buf in = read_file(path_a);
    CHECK_EQ(in.n, orig.n);

    g_logn = 0;
    PgReport rep;
    size_t on = 0;
    uint8_t *out = postgame_patch_save(in.p, in.n, &on, raise, logcb, NULL, &rep);
    CHECK(out != NULL);
    if (!out) return;
    CHECK(rep.recognized);
    Buf patched = {out, on, on};
    write_file(path_b, &patched);
    Buf back = read_file(path_b);
    CHECK_EQ(back.n, on);

    Field a[40], b[40];
    int na = split_fields(&in, a, 40), nb = split_fields(&back, b, 40);
    CHECK_EQ(na, nb);
    CHECK(na >= 14);
    unsigned char ga[4000], gb[4000];
    for (int i = 0; i < na && i < nb; i++) {
        CHECK(!strcmp(a[i].key, b[i].key));
        if (!strcmp(a[i].key, "_global_flag_save")) continue;
        if (!strcmp(a[i].key, "_chapter_number") && raise && chapter < PG_CHAPTER_POSTGAME) { CHECK(!strcmp(b[i].val, "8")); continue; }
        CHECK(!strcmp(a[i].val, b[i].val));   /* every other field equal, character for character */
    }
    int ia = get_field(a, na, "_global_flag_save"), ib = get_field(b, nb, "_global_flag_save");
    CHECK(ia >= 0 && ib >= 0);
    if (ia >= 0 && ib >= 0) {
        int ca = bools(a[ia].val, ga, 4000), cb = bools(b[ib].val, gb, 4000);
        int want_len = ca > 2312 ? ca : 2313;
        CHECK_EQ(cb, want_len);
        for (int i = 0; i < cb; i++) {
            int expected = i < ca ? ga[i] : 0;
            for (int k = 0; k < g_pg_nflags; k++) if (g_pg_flags[k].id == i) expected = g_pg_flags[k].value;
            if (gb[i] != expected) { printf("FAIL flag %d: got %d want %d\n", i, gb[i], expected); g_fail++; }
        }
        CHECK_EQ(rep.set + rep.cleared + rep.already, g_pg_nflags);
        for (int k = 0; k < g_pg_nflags; k++) CHECK_EQ(gb[g_pg_flags[k].id], g_pg_flags[k].value);
    }
    /* the log named every flag exactly once */
    CHECK_EQ(g_logn, g_pg_nflags + (raise && chapter < PG_CHAPTER_POSTGAME ? 1 : 0));   /* one line per flag, plus the chapter line when asked */
    for (int k = 0; k < g_pg_nflags && k < 16; k++) {
        char want[32];
        snprintf(want, sizeof want, "flag 0/%d ", g_pg_flags[k].id);
        CHECK(strstr(g_logs[k], want) != NULL);
    }
    /* applying it again changes nothing (idempotent) */
    size_t n2 = 0;
    PgReport rep2;
    uint8_t *again = postgame_patch_save(back.p, back.n, &n2, raise, NULL, NULL, &rep2);
    CHECK(again != NULL);
    if (again) { CHECK_EQ(n2, back.n); CHECK(!memcmp(again, back.p, n2)); CHECK_EQ(rep2.set + rep2.cleared, 0); free(again); }
    free_fields(a, na); free_fields(b, nb);
    free(in.p); free(orig.p); free(out); free(back.p);
}

/* in-memory form against a fake flag store */
static unsigned char g_store[3000];
static int fget(void *ud, int t, int id) { (void)ud; (void)t; return g_store[id]; }
static void fset(void *ud, int t, int id, int v) { (void)ud; (void)t; g_store[id] = (unsigned char)v; }

static void test_callbacks(void) {
    memset(g_store, 0, sizeof g_store);
    g_store[2311] = 1; g_store[100] = 1;
    g_logn = 0;
    int ch = postgame_apply_flags(fget, fset, NULL, logcb, NULL);
    CHECK_EQ(ch, 5);                       /* 2312, 2025, 2026, 2027 set and 2311 cleared */
    CHECK_EQ(g_logn, g_pg_nflags);
    CHECK_EQ(g_store[2312], 1); CHECK_EQ(g_store[2025], 1); CHECK_EQ(g_store[2026], 1); CHECK_EQ(g_store[2027], 1); CHECK_EQ(g_store[2311], 0);
    CHECK_EQ(g_store[100], 1);              /* untouched */
    g_logn = 0;
    CHECK_EQ(postgame_apply_flags(fget, fset, NULL, logcb, NULL), 0);   /* second time: all already */
}

static void test_option(void) {
    char ini[600];
    snprintf(ini, sizeof ini, "%s\\opt_test.ini", g_dir);
    _putenv_s("SMRPG_UNLOCK_POSTGAME", "");
    CHECK_EQ(postgame_enabled(NULL), 0);
    CHECK_EQ(postgame_enabled("A:\\does\\not\\exist.ini"), 0);
    FILE *f = fopen(ini, "wb");
    fputs("; test\r\n[display]\r\nunlock_postgame = on\r\n[gameplay]\r\n; faithful by default\r\nunlock_postgame = off\r\n", f);
    fclose(f);
    CHECK_EQ(postgame_enabled(ini), 0);   /* a key in the wrong section does not count, off in the right one is off */
    f = fopen(ini, "wb"); fputs("[gameplay]\r\nunlock_postgame = on\r\n", f); fclose(f);
    CHECK_EQ(postgame_enabled(ini), 1);
    f = fopen(ini, "wb"); fputs("[Gameplay]\nUNLOCK_POSTGAME=1\n", f); fclose(f);
    CHECK_EQ(postgame_enabled(ini), 1);
    f = fopen(ini, "wb"); fputs("[gameplay]\nunlock_postgame = maybe\n", f); fclose(f);
    CHECK_EQ(postgame_enabled(ini), 0);   /* unparseable value keeps the faithful default */
    _putenv_s("SMRPG_UNLOCK_POSTGAME", "off");
    f = fopen(ini, "wb"); fputs("[gameplay]\nunlock_postgame = on\n", f); fclose(f);
    CHECK_EQ(postgame_enabled(ini), 0);   /* the environment wins */
    _putenv_s("SMRPG_UNLOCK_POSTGAME", "1");
    CHECK_EQ(postgame_enabled(NULL), 1);
    _putenv_s("SMRPG_UNLOCK_POSTGAME", "");
    CHECK(postgame_is_save_name("SMR_Save_Ver0011000"));
    CHECK(!postgame_is_save_name("SMR_Config"));
    CHECK(!postgame_is_save_name(NULL));
}

static void test_off_and_foreign(void) {
    Buf s = make_save(5, 2400, 0, 1, 0);
    size_t n = 0;
    CHECK(postgame_filter_save(0, s.p, s.n, &n, logcb, NULL) == NULL);     /* off: no bytes read, nothing returned */
    uint8_t *on = postgame_filter_save(1, s.p, s.n, &n, NULL, NULL);
    CHECK(on != NULL); free(on);
    const uint8_t junk[] = {'h', 0, 'e', 0, 'l', 0, 'l', 0, 'o', 0, '!', 0, '!', 0, '!', 0};
    CHECK(postgame_patch_save(junk, sizeof junk, &n, 0, NULL, NULL, NULL) == NULL);   /* not JSON */
    Buf o = {0};
    put(&o, "{\"_coin\":5}");
    CHECK(postgame_patch_save(o.p, o.n, &n, 0, NULL, NULL, NULL) == NULL);            /* JSON without a flag array */
    free(o.p); free(s.p);
}

int main(int argc, char **argv) {
    snprintf(g_dir, sizeof g_dir, "%s", argc > 1 ? argv[1] : ".");
    CreateDirectoryA(g_dir, NULL);
    CHECK_EQ(g_pg_nflags, 5);
    for (int i = 0; i < g_pg_nflags; i++) CHECK(g_pg_flags[i].type == 0 && g_pg_flags[i].name && g_pg_flags[i].why);

    round_trip("compact_mid",    3, 2600, 0, 1, 0, 0);   /* mid-game save, long array */
    round_trip("compact_short",  3, 2000, 0, 1, 0, 0);   /* array shorter than the highest id: padded */
    round_trip("pretty_mid",     5, 2700, 1, 0, 0, 0);   /* pretty-printed, no BOM */
    round_trip("chapter_first",  4, 2600, 0, 1, 0, 1);   /* chapter lifted (owner opt-in), field before the array */
    round_trip("chapter_last",   6, 2600, 0, 1, 1, 1);   /* chapter lifted, field after the array */
    round_trip("post_game_save", 8, 2600, 0, 1, 0, 1);   /* chapter already 8: left alone */
    test_callbacks();
    test_option();
    test_off_and_foreign();
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
