/* Difficulty plus tests (task-25). The retail tables are read as fixtures from A:. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <windows.h>

#include "../../tools/datatbl/nrbf.h"
#include "../../runtime/mods/datatbl_overlay.h"

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a,b) do { long long _a = (long long)(a), _b = (long long)(b); g_checks++; if (_a != _b) { g_failed++; printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

typedef struct Bytes { uint8_t *p; size_t n; } Bytes;

static Bytes read_file(const char *path) {
    Bytes b = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return b;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return b; }
    long length = ftell(f);
    if (length < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return b; }
    b.n = (size_t)length;
    b.p = (uint8_t *)malloc(b.n ? b.n : 1);
    if (!b.p || fread(b.p, 1, b.n, f) != b.n) { free(b.p); b.p = NULL; b.n = 0; }
    fclose(f);
    return b;
}

static int is_monster(const NrbfDocument *doc, const NrbfObject *obj) {
    static const char *const names[] = {"_monster_id", "_hp", "_attack", "_defense", "_magic_attack", "_magic_defense"};
    NrbfInteger v;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!nrbf_object_get_integer(doc, obj, names[i], &v)) return 0;
    return 1;
}

static int64_t numeric_value(NrbfInteger v) { return v.is_signed ? v.signed_value : (int64_t)v.unsigned_value; }

static int64_t expected_scaled(int64_t before, unsigned pct, unsigned bits, int is_signed) {
    int64_t max = is_signed ? ((bits >= 64) ? INT64_MAX : (((int64_t)1 << (bits - 1)) - 1))
                            : ((bits >= 63) ? INT64_MAX : (((int64_t)1 << bits) - 1));
    int64_t scaled = (before * (int64_t)pct + 50) / 100;
    return scaled > max ? max : scaled;
}

static void round_trip_tables_inner(const char *dir, int recursive, unsigned *count, unsigned *decoded) {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    CHECK(find != INVALID_HANDLE_VALUE);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recursive) round_trip_tables_inner(path, 1, count, decoded);
            continue;
        }
        const char *ext = strrchr(fd.cFileName, '.');
        if (!ext || _stricmp(ext, ".tbl")) continue;
        Bytes in = read_file(path);
        NrbfDocument *doc = NULL;
        uint8_t *encoded = NULL;
        size_t encoded_n = 0;
        CHECK(in.p != NULL);
        if (in.p) {
            int valid = nrbf_decode(in.p, in.n, &doc);
            CHECK(valid);
            CHECK(doc != NULL);
            if (doc) {
                CHECK(nrbf_encode(doc, &encoded, &encoded_n));
                CHECK_EQ(encoded_n, in.n);
                CHECK(encoded && encoded_n == in.n && memcmp(encoded, in.p, in.n) == 0);
                (*decoded)++;
            } else {
                printf("UNREADABLE: %s\\%s\n", dir, fd.cFileName);
            }
        }
        free(encoded);
        nrbf_document_free(doc);
        free(in.p);
        (*count)++;
    } while (FindNextFileA(find, &fd));
    FindClose(find);
}

static void round_trip_tables(const char *dir, int recursive, const char *label) {
    unsigned count = 0, decoded = 0;
    round_trip_tables_inner(dir, recursive, &count, &decoded);
    CHECK(count > 0 && decoded == count);
    printf("codec %s: %u of %u tables round-tripped byte-identically\n", label, decoded, count);
}

static void test_all_table_round_trips(const char *dir) {
    round_trip_tables(dir, 0, "top-level");
    round_trip_tables(dir, 1, "recursive");
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    CHECK(fwrite(text, 1, strlen(text), f) == strlen(text));
    CHECK(fclose(f) == 0);
}

static void test_config(const char *scratch) {
    DatatblSettings s;
    datatbl_settings_defaults(&s);
    CHECK_EQ(s.hard, 0);
    CHECK_EQ(s.hp_percent, 150);
    CHECK_EQ(s.damage_percent, 125);
    CHECK_EQ(s.boss_hp_percent, 150);
    CHECK_EQ(s.boss_damage_percent, 125);
    CHECK_EQ(s.boss_id_count, 0);

    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\difficulty_options.ini", scratch);
    const char *text = "[gameplay]\r\n"
        "difficulty_plus = hard\r\n"
        "difficulty_plus_hp_percent = 999\r\n"
        "difficulty_plus_damage_percent = 110\r\n"
        "difficulty_plus_boss_hp_percent = 200\r\n"
        "difficulty_plus_boss_damage_percent = 175\r\n"
        "difficulty_plus_boss_ids = 7, 12, 7\r\n";
    write_text(path, text);
    CHECK(_putenv_s("SMRPG_OPTIONS", path) == 0);
    CHECK(datatbl_settings_load_options(&s));
    CHECK_EQ(s.hard, 1);
    CHECK_EQ(s.hp_percent, DATATBL_SCALE_MAX_PERCENT);
    CHECK_EQ(s.damage_percent, 110);
    CHECK_EQ(s.boss_hp_percent, 200);
    CHECK_EQ(s.boss_damage_percent, 175);
    CHECK_EQ(s.boss_id_count, 2);
    CHECK_EQ(s.boss_ids[0], 7);
    CHECK_EQ(s.boss_ids[1], 12);

    char full_list[DATATBL_BOSS_IDS_MAX * 12 + 80] =
        "[gameplay]\ndifficulty_plus_boss_ids = ";
    size_t length = strlen(full_list);
    const uint32_t first_id = UINT32_MAX - DATATBL_BOSS_IDS_MAX + 1;
    for (unsigned i = 0; i < DATATBL_BOSS_IDS_MAX; i++) {
        int written = snprintf(full_list + length, sizeof full_list - length,
                               "%s%u", i ? ", " : "", (unsigned)(first_id + i));
        CHECK(written > 0 && (size_t)written < sizeof full_list - length);
        if (written <= 0 || (size_t)written >= sizeof full_list - length) break;
        length += (size_t)written;
    }
    write_text(path, full_list);
    CHECK(datatbl_settings_load_options(&s));
    CHECK_EQ(s.boss_id_count, DATATBL_BOSS_IDS_MAX);
    CHECK_EQ(s.boss_ids[0], first_id);
    CHECK_EQ(s.boss_ids[DATATBL_BOSS_IDS_MAX - 1], UINT32_MAX);

    write_text(path, "[gameplay]\ndifficulty_plus_boss_ids = -18446744073709551615\n");
    CHECK(datatbl_settings_load_options(&s));
    CHECK_EQ(s.boss_id_count, 0);

    const char *invalid_mode = "[gameplay]\ndifficulty_plus = turbo\ndifficulty_plus_hp_percent = bad\ndifficulty_plus_boss_ids = not-an-id\n";
    write_text(path, invalid_mode);
    CHECK(datatbl_settings_load_options(&s));
    CHECK_EQ(s.hard, 0);
    CHECK_EQ(s.hp_percent, 150);
    CHECK_EQ(s.boss_id_count, 0);

    CHECK(_putenv_s("SMRPG_OPTIONS", "A:\\AI\\Recomps\\smrpg-work\\build\\datatbl_tests\\missing.ini") == 0);
    CHECK(datatbl_settings_load_options(&s));
    CHECK_EQ(s.hard, 0);
    CHECK_EQ(s.hp_percent, 150);
    CHECK(_putenv_s("SMRPG_OPTIONS", "") == 0);
}

static const NrbfObject *first_monster(const NrbfDocument *doc) {
    for (size_t i = 0; i < nrbf_document_object_count(doc); i++) {
        const NrbfObject *obj = nrbf_document_object_at(doc, i);
        NrbfInteger id;
        if (is_monster(doc, obj) && nrbf_object_get_integer(doc, obj, "_monster_id", &id) && numeric_value(id) > 0) return obj;
    }
    return NULL;
}

static void test_real_monster_table(const char *dir) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\monster.tbl", dir);
    Bytes src = read_file(path);
    CHECK(src.p != NULL);
    if (!src.p) return;

    size_t out_n = 0;
    uint8_t *out = NULL;
    DatatblReport report;
    DatatblSettings cfg;
    datatbl_settings_defaults(&cfg);
    CHECK(datatbl_transform_monster(src.p, src.n, &cfg, &out, &out_n, &report));
    CHECK_EQ(out_n, src.n);
    CHECK(out && out_n == src.n && memcmp(out, src.p, src.n) == 0);
    free(out);

    NrbfDocument *before = NULL, *after = NULL;
    CHECK(nrbf_decode(src.p, src.n, &before));
    const NrbfObject *sample = before ? first_monster(before) : NULL;
    CHECK(sample != NULL);
    NrbfInteger sample_id = {0};
    CHECK(sample && nrbf_object_get_integer(before, sample, "_monster_id", &sample_id));
    if (!sample) { nrbf_document_free(before); free(src.p); return; }

    cfg.hard = 1;
    cfg.hp_percent = 150;
    cfg.damage_percent = 125;
    cfg.boss_hp_percent = 200;
    cfg.boss_damage_percent = 175;
    cfg.boss_ids[0] = (uint32_t)numeric_value(sample_id);
    cfg.boss_id_count = 1;
    CHECK(datatbl_transform_monster(src.p, src.n, &cfg, &out, &out_n, &report));
    CHECK(out && out_n == src.n);
    CHECK(report.monster_count > 0);
    CHECK_EQ(report.boss_count, 1);
    CHECK(report.changed_stats > 0);
    uint8_t *cache = NULL;
    size_t cache_n = 0;
    CHECK(datatbl_overlay_prepare_cache(src.p, src.n, &cfg, src.n, &cache, &cache_n, &report));
    CHECK(cache && cache_n == src.n && memcmp(cache, out, src.n) == 0);
    free(cache);
    cache = NULL;
    CHECK(!datatbl_overlay_prepare_cache(src.p, src.n, &cfg, src.n - 1, &cache, &cache_n, NULL));
    CHECK(cache == NULL);
    CHECK_EQ(cache_n, 0);
    DatatblSettings faithful;
    datatbl_settings_defaults(&faithful);
    CHECK(!datatbl_overlay_prepare_cache(src.p, src.n, &faithful, src.n, &cache, &cache_n, NULL));
    CHECK(cache == NULL);
    CHECK_EQ(cache_n, 0);
    CHECK(nrbf_decode(out, out_n, &after));
    if (before && after) {
        static const char *const stats[] = {"_hp", "_attack", "_defense", "_magic_attack", "_magic_defense"};
        unsigned monsters = 0;
        for (size_t i = 0; i < nrbf_document_object_count(before); i++) {
            const NrbfObject *a = nrbf_document_object_at(before, i);
            if (!is_monster(before, a)) continue;
            NrbfInteger id, exp_a, coin_a;
            CHECK(nrbf_object_get_integer(before, a, "_monster_id", &id));
            CHECK(nrbf_object_get_integer(before, a, "_exp", &exp_a));
            CHECK(nrbf_object_get_integer(before, a, "_coin", &coin_a));
            const NrbfObject *b = nrbf_document_object_at(after, i);
            CHECK(b != NULL);
            if (!b) continue;
            CHECK(is_monster(after, b));
            NrbfInteger id_b;
            CHECK(nrbf_object_get_integer(after, b, "_monster_id", &id_b));
            CHECK_EQ(numeric_value(id_b), numeric_value(id));
            NrbfInteger exp_b, coin_b;
            CHECK(nrbf_object_get_integer(after, b, "_exp", &exp_b));
            CHECK(nrbf_object_get_integer(after, b, "_coin", &coin_b));
            CHECK_EQ(numeric_value(exp_b), numeric_value(exp_a));
            CHECK_EQ(numeric_value(coin_b), numeric_value(coin_a));
            unsigned hp_pct = (uint32_t)numeric_value(id) == cfg.boss_ids[0] ? cfg.boss_hp_percent : cfg.hp_percent;
            unsigned dmg_pct = (uint32_t)numeric_value(id) == cfg.boss_ids[0] ? cfg.boss_damage_percent : cfg.damage_percent;
            for (size_t k = 0; k < sizeof stats / sizeof stats[0]; k++) {
                NrbfInteger x, y;
                CHECK(nrbf_object_get_integer(before, a, stats[k], &x));
                CHECK(nrbf_object_get_integer(after, b, stats[k], &y));
                unsigned pct = k == 0 ? hp_pct : dmg_pct;
                CHECK_EQ(numeric_value(y), expected_scaled(numeric_value(x), pct, x.bits, x.is_signed));
            }
            monsters++;
        }
        CHECK_EQ(monsters, report.monster_count);
    }
    nrbf_document_free(after);
    free(out);

    /* Raising one serialized HP near its signed field limit must saturate at that field's maximum. */
    NrbfObject *mutable_sample = NULL;
    if (before) {
        for (size_t i = 0; i < nrbf_document_object_count(before); i++) {
            NrbfObject *obj = nrbf_document_object_at_mut(before, i);
            if (is_monster(before, obj)) { mutable_sample = obj; break; }
        }
    }
    CHECK(mutable_sample != NULL);
    if (mutable_sample) {
        CHECK(nrbf_object_set_integer(before, mutable_sample, "_hp", INT32_MAX / 2 + 1));
        uint8_t *seed = NULL;
        size_t seed_n = 0;
        CHECK(nrbf_encode(before, &seed, &seed_n));
        datatbl_settings_defaults(&cfg);
        cfg.hard = 1;
        cfg.hp_percent = 400;
        CHECK(datatbl_transform_monster(seed, seed_n, &cfg, &out, &out_n, &report));
        NrbfDocument *clamped = NULL;
        CHECK(nrbf_decode(out, out_n, &clamped));
        size_t sample_index = 0;
        for (; sample_index < nrbf_document_object_count(before); sample_index++)
            if (nrbf_document_object_at_mut(before, sample_index) == mutable_sample) break;
        const NrbfObject *row = clamped ? nrbf_document_object_at(clamped, sample_index) : NULL;
        NrbfInteger hp;
        CHECK(row && nrbf_object_get_integer(clamped, row, "_hp", &hp));
        if (row) CHECK_EQ(numeric_value(hp), INT32_MAX);
        nrbf_document_free(clamped);
        free(seed);
        free(out);
    }

    CHECK(!datatbl_transform_monster((const uint8_t *)"not NRBF", 8, &cfg, &out, &out_n, &report));
    CHECK(out == NULL);
    CHECK_EQ(out_n, 0);
    nrbf_document_free(before);
    free(src.p);
}

int main(int argc, char **argv) {
    if (argc != 3) { printf("usage: test_difficulty_plus DATA_TBL_DIR SCRATCH_DIR\n"); return 2; }
    test_config(argv[2]);
    test_all_table_round_trips(argv[1]);
    test_real_monster_table(argv[1]);
    printf("difficulty_plus: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
