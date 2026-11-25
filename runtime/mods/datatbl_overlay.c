/* Star Road Recomp optional monster table overlay (task-25). */
#include "datatbl_overlay.h"
#include "../../tools/datatbl/nrbf.h"
#include "../platform/overlay/options.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static unsigned clamp_percent(long value) {
    if (value < DATATBL_SCALE_MIN_PERCENT) return DATATBL_SCALE_MIN_PERCENT;
    if (value > DATATBL_SCALE_MAX_PERCENT) return DATATBL_SCALE_MAX_PERCENT;
    return (unsigned)value;
}

void datatbl_settings_defaults(DatatblSettings *settings) {
    if (!settings) return;
    memset(settings, 0, sizeof *settings);
    settings->hp_percent = 150;
    settings->damage_percent = 125;
    settings->boss_hp_percent = 150;
    settings->boss_damage_percent = 125;
}

static int list_has_id(const DatatblSettings *settings, uint32_t id) {
    for (size_t i = 0; i < settings->boss_id_count; i++) if (settings->boss_ids[i] == id) return 1;
    return 0;
}

static int parse_boss_ids(const char *text, uint32_t *ids, size_t *count) {
    char *copy = _strdup(text);
    if (!copy) return 0;
    size_t n = 0;
    char *p = copy;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        if (!isdigit((unsigned char)*p)) { free(copy); return 0; }
        char *end = NULL;
        errno = 0;
        unsigned long long value = _strtoui64(p, &end, 10);
        if (errno || end == p || value > UINT32_MAX || (*end && !isspace((unsigned char)*end) && *end != ',')) { free(copy); return 0; }
        if (n == DATATBL_BOSS_IDS_MAX) { free(copy); return 0; }
        uint32_t id = (uint32_t)value;
        int seen = 0;
        for (size_t i = 0; i < n; i++) if (ids[i] == id) { seen = 1; break; }
        if (!seen) ids[n++] = id;
        p = end;
    }
    free(copy);
    *count = n;
    return 1;
}

static void load_boss_ids_extra(const char *extra, DatatblSettings *settings) {
    if (!extra || !*extra) return;
    char section[64] = "";
    const char *line = extra;
    while (*line) {
        char buffer[OPT_FILE_LINE_MAX];
        size_t n = 0;
        while (line[n] && line[n] != '\n' && n + 1 < sizeof buffer) n++;
        memcpy(buffer, line, n);
        buffer[n] = 0;
        line += n;
        if (*line == '\n') line++;
        char *s = buffer;
        while (*s && isspace((unsigned char)*s)) s++;
        char *end = s + strlen(s);
        while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (close) {
                *close = 0;
                snprintf(section, sizeof section, "%s", s + 1);
            }
        } else if (eq_ci(section, "gameplay")) {
            char *eq = strchr(s, '=');
            if (eq) {
                *eq++ = 0;
                char *key_end = s + strlen(s);
                while (key_end > s && isspace((unsigned char)key_end[-1])) *--key_end = 0;
                while (*eq && isspace((unsigned char)*eq)) eq++;
                if (eq_ci(s, "difficulty_plus_boss_ids")) {
                    uint32_t ids[DATATBL_BOSS_IDS_MAX];
                    size_t count = 0;
                    if (parse_boss_ids(eq, ids, &count)) {
                        memcpy(settings->boss_ids, ids, count * sizeof *ids);
                        settings->boss_id_count = count;
                    }
                }
            }
        }
    }
}

int datatbl_settings_load_options(DatatblSettings *settings) {
    if (!settings) return 0;
    datatbl_settings_defaults(settings);
    Options options;
    opts_defaults(&options);
    char path[OPT_PATH_MAX];
    opts_file_path(path, sizeof path);
    if (opts_load(&options, path) >= 0) {
        settings->hard = opt_get(&options, OPT_DIFFICULTY_PLUS) == 1;
        settings->hp_percent = (unsigned)opt_get(&options, OPT_DIFFICULTY_PLUS_HP_PERCENT);
        settings->damage_percent = (unsigned)opt_get(&options, OPT_DIFFICULTY_PLUS_DAMAGE_PERCENT);
        settings->boss_hp_percent = (unsigned)opt_get(&options, OPT_DIFFICULTY_PLUS_BOSS_HP_PERCENT);
        settings->boss_damage_percent = (unsigned)opt_get(&options, OPT_DIFFICULTY_PLUS_BOSS_DAMAGE_PERCENT);
        load_boss_ids_extra(options.extra, settings);
    }
    opts_free(&options);
    return 1;
}

static int scale_integer(NrbfInteger before, unsigned percent, NrbfInteger *after, int *saturated) {
    uint64_t value, max;
    *after = before;
    *saturated = 0;
    if (before.is_signed) {
        if (before.signed_value < 0) return 1;
        value = (uint64_t)before.signed_value;
        max = before.bits == 64 ? INT64_MAX : (((uint64_t)1 << (before.bits - 1)) - 1);
    } else {
        value = before.unsigned_value;
        max = before.bits == 64 ? UINT64_MAX : (((uint64_t)1 << before.bits) - 1);
    }
    uint64_t q = value / 100, r = value % 100;
    uint64_t scaled, tail = (r * percent + 50) / 100;
    if (q > max / percent) { scaled = max; *saturated = 1; }
    else {
        uint64_t whole = q * percent;
        if (tail > max - whole) { scaled = max; *saturated = 1; }
        else scaled = whole + tail;
    }
    if (before.is_signed) after->signed_value = (int64_t)scaled;
    else after->unsigned_value = scaled;
    return 1;
}

static int is_monster(const NrbfDocument *doc, const NrbfObject *object) {
    static const char *const names[] = {"_monster_id", "_hp", "_attack", "_defense", "_magic_attack", "_magic_defense"};
    NrbfInteger value;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!nrbf_object_get_integer(doc, object, names[i], &value)) return 0;
    return 1;
}

static int contains_partial_monster(const NrbfDocument *doc, const NrbfObject *object) {
    NrbfInteger value;
    if (!nrbf_object_get_integer(doc, object, "_monster_id", &value)) return 0;
    return !is_monster(doc, object);
}

static int copy_passthrough(const uint8_t *input, size_t input_size, uint8_t **out, size_t *out_size) {
    uint8_t *copy = (uint8_t *)malloc(input_size ? input_size : 1);
    if (!copy) return 0;
    if (input_size) memcpy(copy, input, input_size);
    *out = copy;
    *out_size = input_size;
    return 1;
}

int datatbl_transform_monster(const uint8_t *input, size_t input_size, const DatatblSettings *settings,
                              uint8_t **out, size_t *out_size, DatatblReport *report) {
    if (out) *out = NULL;
    if (out_size) *out_size = 0;
    if (report) memset(report, 0, sizeof *report);
    if (!input || !input_size || !settings || !out || !out_size) return 0;
    if (settings->boss_id_count > DATATBL_BOSS_IDS_MAX) return 0;
    if (!settings->hard) return copy_passthrough(input, input_size, out, out_size);

    unsigned hp_percent = clamp_percent((long)settings->hp_percent);
    unsigned damage_percent = clamp_percent((long)settings->damage_percent);
    unsigned boss_hp_percent = clamp_percent((long)settings->boss_hp_percent);
    unsigned boss_damage_percent = clamp_percent((long)settings->boss_damage_percent);
    NrbfDocument *doc = NULL;
    if (!nrbf_decode(input, input_size, &doc)) return 0;
    DatatblReport result = {0};
    static const char *const stats[] = {"_hp", "_attack", "_defense", "_magic_attack", "_magic_defense"};
    for (size_t i = 0; i < nrbf_document_object_count(doc); i++) {
        NrbfObject *object = nrbf_document_object_at_mut(doc, i);
        if (contains_partial_monster(doc, object)) { nrbf_document_free(doc); return 0; }
        if (!is_monster(doc, object)) continue;
        NrbfInteger id_value;
        if (!nrbf_object_get_integer(doc, object, "_monster_id", &id_value)) { nrbf_document_free(doc); return 0; }
        uint64_t monster_id;
        if (id_value.is_signed) {
            if (id_value.signed_value < 0) { nrbf_document_free(doc); return 0; }
            monster_id = (uint64_t)id_value.signed_value;
        } else monster_id = id_value.unsigned_value;
        if (monster_id > UINT32_MAX) { nrbf_document_free(doc); return 0; }
        int boss = list_has_id(settings, (uint32_t)monster_id);
        unsigned hp = boss ? boss_hp_percent : hp_percent;
        unsigned damage = boss ? boss_damage_percent : damage_percent;
        result.monster_count++;
        if (boss) result.boss_count++;
        for (size_t k = 0; k < sizeof stats / sizeof stats[0]; k++) {
            NrbfInteger before, after;
            int saturated = 0;
            if (!nrbf_object_get_integer(doc, object, stats[k], &before) || !scale_integer(before, k == 0 ? hp : damage, &after, &saturated)) {
                nrbf_document_free(doc);
                return 0;
            }
            int same = before.is_signed ? before.signed_value == after.signed_value : before.unsigned_value == after.unsigned_value;
            if (!same) {
                if (!nrbf_object_set_integer_value(doc, object, stats[k], &after)) { nrbf_document_free(doc); return 0; }
                result.changed_stats++;
            }
            if (saturated) result.saturated_stats++;
        }
    }
    if (!result.monster_count || !nrbf_encode(doc, out, out_size) || *out_size != input_size) {
        nrbf_document_free(doc);
        free(*out);
        *out = NULL;
        *out_size = 0;
        return 0;
    }
    nrbf_document_free(doc);
    if (report) *report = result;
    return 1;
}

int datatbl_overlay_prepare_cache(const uint8_t *input, size_t input_size, const DatatblSettings *settings,
                                  size_t max_size, uint8_t **out, size_t *out_size, DatatblReport *report) {
    if (out) *out = NULL;
    if (out_size) *out_size = 0;
    if (report) memset(report, 0, sizeof *report);
    if (!input || !input_size || !settings || !settings->hard || input_size > max_size || !out || !out_size) return 0;
    if (!datatbl_transform_monster(input, input_size, settings, out, out_size, report)) return 0;
    if (*out_size != input_size) {
        free(*out);
        *out = NULL;
        *out_size = 0;
        return 0;
    }
    return 1;
}
