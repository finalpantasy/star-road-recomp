/* Star Road Recomp options store (task-16). Our own code. Pure C: no SDL, no ImGui, so the unit test (tests/platform/test_options.c) links it alone.
 *
 * One table (options.c) describes every option once: key, page, label, type, default, range, whether a change applies live or on restart.
 * The overlay (overlay.cpp) draws pages from the table, the apply layer reads the values. The file is a small ini:
 *   [section]
 *   key = value        ; enum options hold the name ("fsr1"), numbers hold the number
 * Unknown keys are kept and written back, so a file edited by a newer build survives an older one. config/options.default.ini is the
 * documented default file (a test checks that it matches the table). Where the file lives: docs/PLATFORM.md ("Options"). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { OPT_BOOL, OPT_ENUM, OPT_INT, OPT_PATH } OptType;
typedef enum { OPT_LIVE = 0, OPT_RESTART = 1, OPT_ON_LOAD = 2 } OptApply;   /* RESTART: stored now, takes effect the next time the game starts; ON_LOAD: read by the game when it loads a save or starts a new game */

/* ids in table order (the pages follow the table order) */
enum {
    OPT_WINDOW_MODE,       /* display: windowed | fullscreen                        live (only with --window) */
    OPT_UPSCALE,           /* display: off | fsr1 | fsr2 (task-23)                  live */
    OPT_UPSCALE_TARGET,    /* display: window | 1080p | 1440p | 2160p               live */
    OPT_SHARPNESS,         /* display: RCAS sharpening                              live */
    OPT_FPS_CAP,           /* performance: game | 30 | 60 | 90 | 120 | 144 | 240   live */
    OPT_AUDIO,             /* audio: off | on (a real device opens at start)        restart */
    OPT_VOLUME,            /* audio: 0 .. 100                                       live */
    OPT_INPUT_MAP,         /* controls: keyboard map ini ("" = input.ini, else input.default.ini)   live */
    OPT_UNLOCK_POSTGAME,   /* gameplay: off | on (task-24, post-game boss rematches)  on save load or new game */
    OPT_COUNT
};

typedef struct OptDesc {
    const char *section, *key, *page, *label, *help;
    OptType type;
    OptApply apply;
    int def, min, max, step;               /* INT: range and step; ENUM: 0 .. n_names-1; BOOL: 0 | 1 */
    const char *const *names;              /* ENUM: value names (the ini holds these; the first is the default for most) */
    int n_names;
    const char *def_path;                  /* PATH default */
} OptDesc;

#define OPT_PATH_MAX 260
typedef struct Options {
    int v[OPT_COUNT];                      /* BOOL, ENUM, INT values (PATH entries stay 0) */
    char path[OPT_COUNT][OPT_PATH_MAX];    /* PATH values */
    int applied[OPT_COUNT];                /* value the running game started with (restart tracking) */
    char applied_path[OPT_COUNT][OPT_PATH_MAX];
    char *extra;                           /* unknown "[section]\nkey = value" lines of the file read, written back unchanged */
    unsigned revision;                     /* bumps on every change */
} Options;

const OptDesc *opt_desc(int id);
void opts_defaults(Options *o);                              /* also marks the defaults as "applied" */
void opts_free(Options *o);
int opts_load(Options *o, const char *path);                  /* values read (>= 0), -1 if the file cannot be opened; bad lines are skipped; marks "applied" */
int opts_save(const Options *o, const char *path);            /* 0 ok; writes a temp file beside it and renames, so a crash never leaves half a file */
int opt_get(const Options *o, int id);
const char *opt_get_path(const Options *o, int id);
int opt_set(Options *o, int id, int value);                   /* clamps to the range; returns 1 if the value changed */
int opt_set_path(Options *o, int id, const char *value);
int opt_step(Options *o, int id, int dir);                    /* +1 / -1: next / previous (wraps for BOOL and ENUM, clamps for INT); returns 1 if changed */
void opt_reset(Options *o, int id);
int opt_is_default(const Options *o, int id);
int opt_needs_restart(const Options *o, int id);              /* RESTART option whose value differs from what the game started with */
int opts_any_restart(const Options *o);
/* the value as text for the menu: the enum name, "on"/"off", the number, the path */
void opt_text(const Options *o, int id, char *buf, size_t n);
/* the default file text: what config/options.default.ini must contain (the test compares them) */
size_t opts_default_ini(char *buf, size_t n);
/* where the options file lives: SMRPG_OPTIONS, else options.ini beside the exe */
void opts_file_path(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
