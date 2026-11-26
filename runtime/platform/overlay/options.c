/* Star Road Recomp options store (task-16). Our own code. See options.h. */
#include "options.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static const char *const N_WINDOW[] = {"windowed", "fullscreen"};
static const char *const N_UPSCALE[] = {"off", "fsr1", "fsr2"};
static const char *const N_TARGET[] = {"window", "1080p", "1440p", "2160p"};
static const char *const N_SHARP[] = {"off", "sharpest", "strong", "default", "mild", "soft"};
static const char *const N_FPS[] = {"game", "30", "60", "90", "120", "144", "240"};
static const char *const N_OFFON[] = {"off", "on"};
static const char *const N_DIFFICULTY_PLUS[] = {"off", "hard"};
#define NN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* The table. Order = page order in the menu and order in the ini. `page` groups items on one menu page. */
static const OptDesc T[OPT_COUNT] = {
    [OPT_WINDOW_MODE] = {"display", "window_mode", "Display", "Window mode",
        "windowed or fullscreen (borderless desktop). Applies live when the window is shown (--window).",
        OPT_ENUM, OPT_LIVE, 0, 0, 1, 1, N_WINDOW, NN(N_WINDOW), 0},
    [OPT_UPSCALE] = {"display", "upscale", "Display", "Upscaling",
        "off is the faithful picture (a plain scale). fsr1 and fsr2 are AMD FidelityFX spatial upscalers.",
        OPT_ENUM, OPT_LIVE, 0, 0, 2, 1, N_UPSCALE, NN(N_UPSCALE), 0},
    [OPT_UPSCALE_TARGET] = {"display", "upscale_target", "Display", "Output size",
        "size of the upscaled picture: window (the window size) or a fixed 1080p, 1440p or 2160p image.",
        OPT_ENUM, OPT_LIVE, 0, 0, 3, 1, N_TARGET, NN(N_TARGET), 0},
    [OPT_SHARPNESS] = {"display", "sharpness", "Display", "Sharpening",
        "RCAS sharpening after upscaling: off, or from sharpest to soft. Only used when upscaling is on.",
        OPT_ENUM, OPT_LIVE, 3, 0, 5, 1, N_SHARP, NN(N_SHARP), 0},
    [OPT_FPS_CAP] = {"performance", "fps_cap", "Performance", "Frame rate cap",
        "game adds no limit (the game's own pacing and vsync). A number caps presented frames per second.",
        OPT_ENUM, OPT_LIVE, 0, 0, 6, 1, N_FPS, NN(N_FPS), 0},
    [OPT_AUDIO] = {"audio", "enabled", "Audio", "Audio output",
        "on opens the real playback device when audout starts. A running null sink cannot be promoted to a device, so restart is required.",
        OPT_BOOL, OPT_RESTART, 0, 0, 1, 1, N_OFFON, NN(N_OFFON), 0},
    [OPT_VOLUME] = {"audio", "volume", "Audio", "Volume",
        "master volume of the audio output, 0 to 100.",
        OPT_INT, OPT_LIVE, 100, 0, 100, 5, NULL, 0, 0},
    [OPT_INPUT_MAP] = {"controls", "input_map", "Controls", "Keyboard map",
        "ini file with the keyboard bindings (config/input.default.ini shows the format). Empty: input.ini beside the exe, else input.default.ini.",
        OPT_PATH, OPT_LIVE, 0, 0, 0, 0, NULL, 0, ""},
    [OPT_UNLOCK_POSTGAME] = {"gameplay", "unlock_postgame", "Gameplay", "Unlock post-game",
        "off is the game as shipped. on unlocks the post-game boss rematches without playing through. Applies on save load or new game.",
        OPT_BOOL, OPT_ON_LOAD, 0, 0, 1, 1, N_OFFON, NN(N_OFFON), 0},
    [OPT_DIFFICULTY_PLUS] = {"gameplay", "difficulty_plus", "Gameplay", "Difficulty Plus",
        "off is faithful. hard raises monster stats from the data table. The runtime caches the transformed monster.tbl once per run, so restart is required.",
        OPT_ENUM, OPT_RESTART, 0, 0, 1, 1, N_DIFFICULTY_PLUS, NN(N_DIFFICULTY_PLUS), 0},
    [OPT_DIFFICULTY_PLUS_HP_PERCENT] = {"gameplay", "difficulty_plus_hp_percent", "Gameplay", "Enemy HP scale (%)",
        "hard mode HP multiplier, from 100% to 400%. Default 150%. monster.tbl is cached once per run, so restart is required.",
        OPT_INT, OPT_RESTART, 150, 100, 400, 5, NULL, 0, 0},
    [OPT_DIFFICULTY_PLUS_DAMAGE_PERCENT] = {"gameplay", "difficulty_plus_damage_percent", "Gameplay", "Enemy stat scale (%)",
        "hard mode attack, defense, magic attack and magic defense multiplier. Default 125%. monster.tbl is cached once per run, so restart is required.",
        OPT_INT, OPT_RESTART, 125, 100, 400, 5, NULL, 0, 0},
    [OPT_DIFFICULTY_PLUS_BOSS_HP_PERCENT] = {"gameplay", "difficulty_plus_boss_hp_percent", "Gameplay", "Boss HP scale (%)",
        "HP multiplier for monster IDs listed in difficulty_plus_boss_ids in options.ini. Default 150%. monster.tbl is cached once per run, so restart is required.",
        OPT_INT, OPT_RESTART, 150, 100, 400, 5, NULL, 0, 0},
    [OPT_DIFFICULTY_PLUS_BOSS_DAMAGE_PERCENT] = {"gameplay", "difficulty_plus_boss_damage_percent", "Gameplay", "Boss stat scale (%)",
        "Attack, defense, magic attack and magic defense multiplier for IDs in difficulty_plus_boss_ids. Default 125%. monster.tbl is cached once per run, so restart is required.",
        OPT_INT, OPT_RESTART, 125, 100, 400, 5, NULL, 0, 0},
};

const OptDesc *opt_desc(int id) { return (id >= 0 && id < OPT_COUNT) ? &T[id] : NULL; }

static void mark_applied(Options *o) {
    memcpy(o->applied, o->v, sizeof o->applied);
    memcpy(o->applied_path, o->path, sizeof o->applied_path);
}

void opts_defaults(Options *o) {
    memset(o, 0, sizeof *o);
    for (int i = 0; i < OPT_COUNT; i++) {
        o->v[i] = T[i].def;
        if (T[i].type == OPT_PATH) snprintf(o->path[i], OPT_PATH_MAX, "%s", T[i].def_path ? T[i].def_path : "");
    }
    mark_applied(o);
}

void opts_free(Options *o) { free(o->extra); o->extra = NULL; }
int opt_get(const Options *o, int id) { return o->v[id]; }
const char *opt_get_path(const Options *o, int id) { return o->path[id]; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int opt_set(Options *o, int id, int value) {
    const OptDesc *d = &T[id];
    if (d->type == OPT_PATH) return 0;
    if (d->type == OPT_INT) value = clampi(value, d->min, d->max);
    else value = clampi(value, 0, d->type == OPT_BOOL ? 1 : d->n_names - 1);
    if (o->v[id] == value) return 0;
    o->v[id] = value;
    o->revision++;
    return 1;
}

int opt_set_path(Options *o, int id, const char *value) {
    if (T[id].type != OPT_PATH) return 0;
    if (!value) value = "";
    if (!strcmp(o->path[id], value)) return 0;
    snprintf(o->path[id], OPT_PATH_MAX, "%s", value);
    o->revision++;
    return 1;
}

int opt_step(Options *o, int id, int dir) {
    const OptDesc *d = &T[id];
    int v = o->v[id];
    if (d->type == OPT_INT) return opt_set(o, id, v + dir * d->step);
    if (d->type == OPT_PATH) return 0;
    int n = d->type == OPT_BOOL ? 2 : d->n_names;
    return opt_set(o, id, ((v + dir) % n + n) % n);
}

void opt_reset(Options *o, int id) {
    if (T[id].type == OPT_PATH) opt_set_path(o, id, T[id].def_path);
    else opt_set(o, id, T[id].def);
}

int opt_is_default(const Options *o, int id) {
    if (T[id].type == OPT_PATH) return !strcmp(o->path[id], T[id].def_path ? T[id].def_path : "");
    return o->v[id] == T[id].def;
}

int opt_needs_restart(const Options *o, int id) {
    if (T[id].apply != OPT_RESTART) return 0;
    if (T[id].type == OPT_PATH) return strcmp(o->path[id], o->applied_path[id]) != 0;
    return o->v[id] != o->applied[id];
}

int opts_any_restart(const Options *o) {
    for (int i = 0; i < OPT_COUNT; i++) if (opt_needs_restart(o, i)) return 1;
    return 0;
}

void opt_text(const Options *o, int id, char *buf, size_t n) {
    const OptDesc *d = &T[id];
    if (d->type == OPT_PATH) {
        const char *p = o->path[id];
        if (!*p) { snprintf(buf, n, "auto"); return; }
        const char *slash = strrchr(p, '\\');
        const char *slash2 = strrchr(p, '/');
        if (slash2 && (!slash || slash2 > slash)) slash = slash2;
        snprintf(buf, n, "%s", slash ? slash + 1 : p);
    } else if (d->type == OPT_INT) {
        snprintf(buf, n, "%d", o->v[id]);
    } else {
        snprintf(buf, n, "%s", d->names[clampi(o->v[id], 0, d->n_names - 1)]);
    }
}

/* ---------------------------------------------------------------- ini */

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = 0;
    return s;
}

static int parse_bool(const char *s, int *out) {
    if (!_stricmp(s, "on") || !_stricmp(s, "true") || !_stricmp(s, "yes") || !strcmp(s, "1")) { *out = 1; return 0; }
    if (!_stricmp(s, "off") || !_stricmp(s, "false") || !_stricmp(s, "no") || !strcmp(s, "0")) { *out = 0; return 0; }
    return -1;
}

static int find_opt(const char *section, const char *key) {
    for (int i = 0; i < OPT_COUNT; i++)
        if (!_stricmp(T[i].section, section) && !_stricmp(T[i].key, key)) return i;
    return -1;
}

static void add_extra(Options *o, const char *section, const char *key, const char *val) {
    size_t have = o->extra ? strlen(o->extra) : 0, add = strlen(section) + strlen(key) + strlen(val) + 8;
    char *n = (char *)realloc(o->extra, have + add);
    if (!n) return;
    o->extra = n;
    snprintf(n + have, add, "[%s]\n%s = %s\n", section, key, val);
}

static int apply_value(Options *o, int id, const char *val) {
    const OptDesc *d = &T[id];
    int v;
    switch (d->type) {
    case OPT_PATH: snprintf(o->path[id], OPT_PATH_MAX, "%s", val); return 0;
    case OPT_BOOL: if (parse_bool(val, &v)) return -1; o->v[id] = v; return 0;
    case OPT_INT: {
        char *end;
        long x = strtol(val, &end, 10);
        if (end == val || *end) return -1;
        o->v[id] = clampi((int)x, d->min, d->max);
        return 0;
    }
    case OPT_ENUM:
        for (int i = 0; i < d->n_names; i++) if (!_stricmp(val, d->names[i])) { o->v[id] = i; return 0; }
        return -1;
    }
    return -1;
}

int opts_load(Options *o, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    opts_defaults(o);                                   /* initialises the struct: free an earlier one with opts_free first */
    char line[OPT_FILE_LINE_MAX], section[64] = "";
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n') && strlen(line) == sizeof line - 1) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
            continue;
        }
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = 0; snprintf(section, sizeof section, "%s", trim(s + 1)); }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        int id = find_opt(section, key);
        if (id >= 0 && T[id].type != OPT_PATH) {
            char *c = strstr(val, " ;");
            if (c) { *c = 0; val = trim(val); }
        }
        if (id < 0) { add_extra(o, section, key, val); continue; }
        if (apply_value(o, id, val) == 0) n++;
    }
    fclose(f);
    mark_applied(o);
    o->revision++;
    return n;
}

/* Disk reloads are stricter than the original startup parser: a malformed field cannot
 * silently clamp a running setting to a different value. Valid fields still merge. */
int opts_reload_live(Options *o, const char *path, int *invalid_count) {
    if (invalid_count) *invalid_count = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    Options parsed;
    opts_defaults(&parsed);
    int valid[OPT_COUNT] = {0}, n = 0, bad = 0;
    char line[OPT_FILE_LINE_MAX], section[64] = "";
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n') && strlen(line) == sizeof line - 1) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
            bad++;
            continue;
        }
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = 0; snprintf(section, sizeof section, "%s", trim(s + 1)); }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        int id = find_opt(section, key);
        if (id < 0) { add_extra(&parsed, section, key, val); continue; }
        if (T[id].type != OPT_PATH) {
            char *c = strstr(val, " ;");
            if (c) { *c = 0; val = trim(val); }
        }
        int ok = 1, value = 0;
        const OptDesc *d = &T[id];
        if (d->type == OPT_PATH) ok = strlen(val) < OPT_PATH_MAX;
        else if (d->type == OPT_BOOL) ok = parse_bool(val, &value) == 0;
        else if (d->type == OPT_ENUM) {
            ok = 0;
            for (int i = 0; i < d->n_names; i++) if (!_stricmp(val, d->names[i])) { value = i; ok = 1; break; }
        } else {
            char *end = NULL;
            errno = 0;
            long x = strtol(val, &end, 10);
            ok = !errno && end != val && *end == 0 && x >= d->min && x <= d->max;
            if (ok) value = (int)x;
        }
        if (!ok) { bad++; continue; }
        if (d->type == OPT_PATH) snprintf(parsed.path[id], OPT_PATH_MAX, "%s", val);
        else parsed.v[id] = value;
        valid[id] = 1;
        n++;
    }
    fclose(f);
    for (int i = 0; i < OPT_COUNT; i++) if (valid[i]) {
        if (T[i].type == OPT_PATH) opt_set_path(o, i, parsed.path[i]);
        else opt_set(o, i, parsed.v[i]);
    }
    free(o->extra);
    o->extra = parsed.extra;
    parsed.extra = NULL;
    opts_free(&parsed);
    if (invalid_count) *invalid_count = bad;
    return n;
}

uint64_t opts_file_stamp(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 0;
    ULARGE_INTEGER t, size;
    t.HighPart = d.ftLastWriteTime.dwHighDateTime;
    t.LowPart = d.ftLastWriteTime.dwLowDateTime;
    size.HighPart = d.nFileSizeHigh;
    size.LowPart = d.nFileSizeLow;
    return t.QuadPart ^ (size.QuadPart * 0x9E3779B185EBCA87ull);
}

void opts_watch_init(OptFileWatch *w, uint64_t stamp, uint64_t now_ms) {
    memset(w, 0, sizeof *w);
    w->observed_stamp = w->applied_stamp = stamp;
    w->changed_at_ms = now_ms;
}

int opts_watch_poll(OptFileWatch *w, uint64_t stamp, uint64_t now_ms, uint64_t debounce_ms) {
    if (stamp != w->observed_stamp) {
        w->observed_stamp = stamp;
        w->changed_at_ms = now_ms;
        w->pending = 1;
    }
    if (!w->pending || now_ms - w->changed_at_ms < debounce_ms) return 0;
    w->pending = 0;
    if (stamp == w->applied_stamp) return 0;
    w->applied_stamp = stamp;
    return 1;
}

void opts_apply_diff(const Options *before, const Options *after, OptApplyFn apply, void *context) {
    if (!before || !after || !apply) return;
    for (int i = 0; i < OPT_COUNT; i++) {
        int changed = T[i].type == OPT_PATH ? strcmp(before->path[i], after->path[i]) != 0 : before->v[i] != after->v[i];
        if (changed) apply(i, context);
    }
}

/* the text of the file for these values (buf may be NULL: only the length) */
static size_t render(const Options *o, char *buf, size_t cap, int with_extra) {
    size_t len = 0;
#define PUT(...) do { int w_ = snprintf(buf ? buf + len : NULL, buf ? (cap > len ? cap - len : 0) : 0, __VA_ARGS__); if (w_ > 0) len += (size_t)w_; } while (0)
    PUT("; Star Road Recomp options. The in-game overlay writes this file (F1, or L + R + MINUS on a controller); you can also edit it while the game runs (most options apply live, the rest say so).\n");
    PUT("; Copy config/options.default.ini to options.ini beside smrpg.exe, or point SMRPG_OPTIONS at a file. A missing key keeps its default.\n");
    const char *last = "";
    for (int i = 0; i < OPT_COUNT; i++) {
        const OptDesc *d = &T[i];
        if (strcmp(last, d->section)) { PUT("\n[%s]\n", d->section); last = d->section; }
        if (d->apply == OPT_ON_LOAD) PUT("; %s\n", d->help);       /* the help text already says when it applies */
        else PUT("; %s (%s)\n", d->help, d->apply == OPT_RESTART ? "applies on restart" : "applies live");
        if (d->type == OPT_ENUM || d->type == OPT_BOOL) {
            PUT("; values:");
            for (int k = 0; k < d->n_names; k++) PUT(" %s", d->names[k]);
            PUT("\n");
        } else if (d->type == OPT_INT) {
            PUT("; range: %d to %d\n", d->min, d->max);
        }
        char val[OPT_PATH_MAX];
        if (d->type == OPT_PATH) snprintf(val, sizeof val, "%s", o->path[i]);
        else if (d->type == OPT_INT) snprintf(val, sizeof val, "%d", o->v[i]);
        else snprintf(val, sizeof val, "%s", d->names[clampi(o->v[i], 0, d->n_names - 1)]);
        PUT("%s = %s\n", d->key, val);
    }
    PUT("; Optional boss-specific factors use an explicit comma-separated monster ID list: difficulty_plus_boss_ids = 12, 18\n");
    if (with_extra && o->extra && *o->extra) { PUT("\n; keys this build does not know (kept as they were)\n"); PUT("%s", o->extra); }
#undef PUT
    return len;
}

size_t opts_default_ini(char *buf, size_t n) {
    Options o;
    opts_defaults(&o);
    size_t len = render(&o, buf, n, 0);
    if (buf && n) buf[len < n ? len : n - 1] = 0;
    return len;
}

int opts_save(const Options *o, const char *path) {
    size_t need = render(o, NULL, 0, 1);
    char *text = (char *)malloc(need + 1);
    if (!text) return -1;
    render(o, text, need + 1, 1);
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(text); return -1; }
    size_t w = fwrite(text, 1, need, f);
    int cl = fclose(f);
    free(text);
    if (w != need || cl) { DeleteFileA(tmp); return -1; }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp); return -1; }
    return 0;
}

void opts_file_path(char *buf, size_t n) {
    const char *env = getenv("SMRPG_OPTIONS");
    if (env && *env) { snprintf(buf, n, "%s", env); return; }
    char exe[MAX_PATH];
    DWORD k = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!k || k >= sizeof exe) { snprintf(buf, n, "options.ini"); return; }
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    snprintf(buf, n, "%s\\options.ini", exe);
}
