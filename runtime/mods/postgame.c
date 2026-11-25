/* Star Road Recomp: option unlock_postgame (task-24). Our own code. See postgame.h and docs/analysis/postgame-flags.md. */
#include "postgame.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The gate, from the developer's own post-game jump rows (event 56 to 58) and the field scripts that read them. All global flags (type 0).
 * 2311 is the "ending save pending" marker; the Mario's-house event clears it when it hands out the ticket, so the unlocked state clears it too. */
const PgFlag g_pg_flags[] = {
    {0, 2312, 1, "game_clear",   "Menu.get_is_gameclear_enable reads it; the ending sets it"},
    {0, 2311, 0, "ed_save_open", "marker between the ending save and the suite ticket; the ticket event clears it"},
    {0, 2025, 1, "suite_ticket", "ticket for the Marrymore hotel suite, given in the Mushroom Kingdom house after the ending save"},
    {0, 2026, 1, "suite_stayed",  "set by the suite stay; the Star Hill and Monstro Town scripts read it"},
    {0, 2027, 1, "star_hill",     "set at Star Hill; the Monstro Town scripts show the rematch hosts only when it is on"},
};
const int g_pg_nflags = (int)(sizeof g_pg_flags / sizeof g_pg_flags[0]);

static void logf_(PgLog log, void *ud, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void logf_(PgLog log, void *ud, const char *fmt, ...) {
    if (!log) return;
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    log(ud, b);
}

/* ---- the option ---- */
static int parse_bool(const char *s, int *out) {
    while (*s == ' ' || *s == '\t') s++;
    char w[16];
    size_t i = 0;
    while (*s && !isspace((unsigned char)*s) && i + 1 < sizeof w) w[i++] = (char)tolower((unsigned char)*s++);
    w[i] = 0;
    if (!strcmp(w, "1") || !strcmp(w, "on") || !strcmp(w, "true") || !strcmp(w, "yes")) { *out = 1; return 1; }
    if (!strcmp(w, "0") || !strcmp(w, "off") || !strcmp(w, "false") || !strcmp(w, "no")) { *out = 0; return 1; }
    return 0;
}

int postgame_enabled(const char *ini_path) {
    int v = 0;
    const char *e = getenv("SMRPG_UNLOCK_POSTGAME");
    if (e && *e && parse_bool(e, &v)) return v;
    if (!ini_path || !*ini_path) return 0;
    FILE *f = fopen(ini_path, "rb");
    if (!f) return 0;
    char line[512];
    int in_sec = 0, result = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '#' || *p == '\r' || *p == '\n' || !*p) continue;
        if (*p == '[') {
            char *r = strchr(p, ']');
            in_sec = r && (size_t)(r - p - 1) == 8 && !_strnicmp(p + 1, "gameplay", 8);
            continue;
        }
        if (!in_sec) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        char *k = p, *ke = eq;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        if ((size_t)(ke - k) == 15 && !_strnicmp(k, "unlock_postgame", 15)) {
            int b;
            if (parse_bool(eq + 1, &b)) result = b;
        }
    }
    fclose(f);
    return result;
}

/* ---- callback form ---- */
int postgame_apply_flags(PgGetFlag get, PgSetFlag set, void *ud, PgLog log, void *log_ud) {
    int changed = 0;
    for (int i = 0; i < g_pg_nflags; i++) {
        const PgFlag *f = &g_pg_flags[i];
        int cur = get(ud, f->type, f->id) ? 1 : 0;
        if (cur == f->value) {
            logf_(log, log_ud, "postgame: flag %d/%d %s already %d", f->type, f->id, f->name, cur);
            continue;
        }
        set(ud, f->type, f->id, f->value);
        changed++;
        logf_(log, log_ud, "postgame: flag %d/%d %s %d -> %d", f->type, f->id, f->name, cur, f->value);
    }
    return changed;
}

/* ---- save file form ---- */
typedef struct W { const uint8_t *b; size_t n; } W;   /* UTF-16LE view: character i is the 16-bit unit at b + 2i */
static unsigned ch(const W *w, size_t i) { return i * 2 + 1 < w->n ? (unsigned)(w->b[i * 2] | (w->b[i * 2 + 1] << 8)) : 0; }
static size_t nch(const W *w) { return w->n / 2; }

static size_t find_ascii(const W *w, size_t from, const char *s) {
    size_t L = strlen(s), N = nch(w);
    for (size_t i = from; i + L <= N; i++) {
        size_t k = 0;
        while (k < L && ch(w, i + k) == (unsigned char)s[k]) k++;
        if (k == L) return i;
    }
    return (size_t)-1;
}
static int is_ws(unsigned c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static size_t skip_ws(const W *w, size_t i) { while (i < nch(w) && is_ws(ch(w, i))) i++; return i; }

/* one boolean element of the flag array */
typedef struct Tok { size_t start, end; int val; } Tok;

static int put_ascii(uint8_t **o, size_t *n, size_t *cap, const char *s) {
    size_t L = strlen(s);
    if (*n + L * 2 > *cap) { *cap = (*n + L * 2) * 2; uint8_t *t = realloc(*o, *cap); if (!t) return 0; *o = t; }
    for (size_t i = 0; i < L; i++) { (*o)[(*n)++] = (uint8_t)s[i]; (*o)[(*n)++] = 0; }
    return 1;
}
static int put_raw(uint8_t **o, size_t *n, size_t *cap, const uint8_t *p, size_t len) {
    if (*n + len > *cap) { *cap = (*n + len) * 2; uint8_t *t = realloc(*o, *cap); if (!t) return 0; *o = t; }
    memcpy(*o + *n, p, len); *n += len; return 1;
}

uint8_t *postgame_patch_save(const uint8_t *in, size_t n, size_t *out_n, int raise_chapter, PgLog log, void *log_ud, PgReport *rep) {
    PgReport r = {0, 0, 0, 0, 0, -1, -1};
    if (rep) *rep = r;
    if (!in || n < 8) return NULL;
    W w = {in, n};
    size_t base = 0;
    if (ch(&w, 0) == 0xFEFF) base = 1;
    if (ch(&w, base) != '{') return NULL;   /* not a JSON object: leave it alone */

    size_t key = find_ascii(&w, base, "\"_global_flag_save\"");
    if (key == (size_t)-1) return NULL;
    size_t p = skip_ws(&w, key + strlen("\"_global_flag_save\""));
    if (ch(&w, p) != ':') return NULL;
    p = skip_ws(&w, p + 1);
    if (ch(&w, p) != '[') return NULL;
    p++;

    /* parse the boolean array, keeping where each token sits so unchanged text is copied as it was */
    size_t cap_t = 4096, nt = 0;
    Tok *t = malloc(cap_t * sizeof *t);
    size_t arr_close = (size_t)-1;
    if (!t) return NULL;
    for (;;) {
        p = skip_ws(&w, p);
        unsigned c = ch(&w, p);
        if (c == ']') { arr_close = p; break; }
        if (c == ',') { p++; continue; }
        int val;
        if (c == 't' && find_ascii(&w, p, "true") == p) val = 1;
        else if (c == 'f' && find_ascii(&w, p, "false") == p) val = 0;
        else { free(t); return NULL; }    /* anything else: not the array we know */
        if (nt == cap_t) { cap_t *= 2; Tok *nt2 = realloc(t, cap_t * sizeof *t); if (!nt2) { free(t); return NULL; } t = nt2; }
        t[nt].start = p; t[nt].val = val; p += val ? 4 : 5; t[nt].end = p; nt++;
    }
    r.recognized = 1;

    /* decide the new values */
    int *want = malloc((nt + 4096) * sizeof *want);
    if (!want) { free(t); return NULL; }
    size_t total = nt;
    for (size_t i = 0; i < nt; i++) want[i] = t[i].val;
    for (int i = 0; i < g_pg_nflags; i++) {
        const PgFlag *f = &g_pg_flags[i];
        if (f->type != 0) continue;   /* the list is all global; area and local flag arrays are not touched */
        size_t id = (size_t)f->id;
        if (id >= total) {
            if (!f->value) { r.already++; logf_(log, log_ud, "postgame: flag 0/%d %s already 0 (beyond array)", f->id, f->name); continue; }
            if (id >= nt + 4096) continue;
            for (size_t k = total; k <= id; k++) want[k] = 0;
            r.padded += (int)(id + 1 - total);
            total = id + 1;
        }
        if (want[id] == f->value) { r.already++; logf_(log, log_ud, "postgame: flag 0/%d %s already %d", f->id, f->name, f->value); continue; }
        logf_(log, log_ud, "postgame: flag 0/%d %s %d -> %d", f->id, f->name, want[id], f->value);
        want[id] = f->value;
        if (f->value) r.set++; else r.cleared++;
    }

    /* chapter field */
    size_t ch_val_s = (size_t)-1, ch_val_e = 0;
    int chapter_new = -1;
    size_t ck = find_ascii(&w, base, "\"_chapter_number\"");
    if (ck != (size_t)-1) {
        size_t q = skip_ws(&w, ck + strlen("\"_chapter_number\""));
        if (ch(&w, q) == ':') {
            q = skip_ws(&w, q + 1);
            size_t s = q;
            int v = 0, neg = 0;
            if (ch(&w, q) == '-') { neg = 1; q++; }
            size_t ds = q;
            while (ch(&w, q) >= '0' && ch(&w, q) <= '9') { v = v * 10 + (int)(ch(&w, q) - '0'); q++; }
            if (q > ds) {
                if (neg) v = -v;
                r.chapter_before = r.chapter_after = v;
                ch_val_s = s; ch_val_e = q;
                if (raise_chapter && v < PG_CHAPTER_POSTGAME) {
                    chapter_new = PG_CHAPTER_POSTGAME;
                    r.chapter_after = chapter_new;
                    logf_(log, log_ud, "postgame: _chapter_number %d -> %d (story skip, asked for)", v, chapter_new);
                }
            }
        }
    }

    /* rebuild from one ordered edit list: [start,end) of the input replaced by text; all other characters are copied as they were */
    size_t cap = n + 64 + (total - nt) * 12, on = 0, copied = 0;
    uint8_t *o = malloc(cap);
    int ok = o != NULL;
    int chapter_done = chapter_new < 0;
    for (size_t i = 0; i < nt && ok; i++) {
        if (!chapter_done && ch_val_s < t[i].start) {   /* the chapter field sits before this token */
            ok = put_raw(&o, &on, &cap, in + copied * 2, (ch_val_s - copied) * 2) && put_ascii(&o, &on, &cap, "8");
            copied = ch_val_e; chapter_done = 1;
        }
        if (want[i] == t[i].val || !ok) continue;
        ok = put_raw(&o, &on, &cap, in + copied * 2, (t[i].start - copied) * 2) && put_ascii(&o, &on, &cap, want[i] ? "true" : "false");
        copied = t[i].end;
    }
    if (ok && !chapter_done && ch_val_s < arr_close) {   /* chapter inside or before the closing bracket (empty array) */
        ok = put_raw(&o, &on, &cap, in + copied * 2, (ch_val_s - copied) * 2) && put_ascii(&o, &on, &cap, "8");
        copied = ch_val_e; chapter_done = 1;
    }
    if (ok && total > nt) {
        ok = put_raw(&o, &on, &cap, in + copied * 2, (arr_close - copied) * 2);
        copied = arr_close;
        for (size_t k = nt; ok && k < total; k++)
            ok = put_ascii(&o, &on, &cap, (k > 0) ? "," : "") && put_ascii(&o, &on, &cap, want[k] ? "true" : "false");
    }
    if (ok && !chapter_done) {   /* chapter after the array */
        ok = put_raw(&o, &on, &cap, in + copied * 2, (ch_val_s - copied) * 2) && put_ascii(&o, &on, &cap, "8");
        copied = ch_val_e;
    }
    if (ok) ok = put_raw(&o, &on, &cap, in + copied * 2, n - copied * 2);
    free(t); free(want);
    if (!ok) { free(o); return NULL; }
    if (rep) *rep = r;
    if (out_n) *out_n = on;
    return o;
}

uint8_t *postgame_filter_save(int enabled, const uint8_t *in, size_t n, size_t *out_n, PgLog log, void *log_ud) {
    if (!enabled) return NULL;
    PgReport rep;
    uint8_t *o = postgame_patch_save(in, n, out_n, 0, log, log_ud, &rep);
    if (!o) logf_(log, log_ud, "postgame: save not recognized, passed through unchanged");
    return o;
}

int postgame_is_save_name(const char *base) {
    return base && !_strnicmp(base, "SMR_Save", 8);
}
