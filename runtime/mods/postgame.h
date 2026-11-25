/* Star Road Recomp: option unlock_postgame (task-24). Our own code.
 *
 * Makes the post-game boss rematches reachable without playing through. The gate is four global flags in the save (docs/analysis/postgame-flags.md);
 * this module sets exactly those, at one defined point: when the game loads its save. With the option off nothing here runs and nothing changes.
 *
 * Two layers:
 *   - postgame_apply_flags: the flag list against a get/set callback pair (what an in-memory hook on GlobalWork.set_global_flag would use).
 *   - postgame_patch_save: the same list applied to the bytes of a save file (SMR_Save JSON, UTF-16LE) as the game reads it. The file on
 *     disk is not touched; only the view the game gets changes. Every byte outside the flag array (and the chapter field, when asked) is kept.
 */
#ifndef SMRPG_MODS_POSTGAME_H
#define SMRPG_MODS_POSTGAME_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PgFlag {
    int type;          /* GlobalWork flag type: 0 global, 1 area, 2 local */
    int id;            /* flag index */
    int value;         /* 1 set, 0 clear */
    const char *name;  /* short label, never game text */
    const char *why;   /* what it gates */
} PgFlag;

extern const PgFlag g_pg_flags[];
extern const int g_pg_nflags;
#define PG_CHAPTER_POSTGAME 8   /* GlobalWork.chapter_number whose script group holds the rematch scripts (unconfirmed, see the doc) */

/* The option: SMRPG_UNLOCK_POSTGAME (0/1/off/on) wins, else key unlock_postgame in section [gameplay] of ini_path (NULL: not read), else off. */
int postgame_enabled(const char *ini_path);

typedef void (*PgLog)(void *ud, const char *line);   /* one call per flag decision; NULL: silent */
typedef struct PgReport {
    int recognized;      /* the bytes looked like a save with a global flag array */
    int set, cleared;    /* flags that changed */
    int already;         /* flags that already had the wanted value */
    int padded;          /* flag array entries appended to reach the highest id */
    int chapter_before, chapter_after;   /* -1 when the field is absent */
} PgReport;

typedef int (*PgGetFlag)(void *ud, int type, int id);
typedef void (*PgSetFlag)(void *ud, int type, int id, int value);
/* Applies g_pg_flags through the callbacks, logs each. Returns how many flags changed. */
int postgame_apply_flags(PgGetFlag get, PgSetFlag set, void *ud, PgLog log, void *log_ud);

/* Applies the list to a save file image (UTF-16LE JSON, optional BOM). Returns a malloc'd patched copy (free() it), or NULL when the bytes are not
 * a save (nothing to do; use the original). raise_chapter: also lift _chapter_number to PG_CHAPTER_POSTGAME (a story skip: off unless the owner says). */
uint8_t *postgame_patch_save(const uint8_t *in, size_t n, size_t *out_n, int raise_chapter, PgLog log, void *log_ud, PgReport *rep);

/* The gate the runtime calls: enabled == 0 returns NULL without reading the bytes ("off changes nothing"). */
uint8_t *postgame_filter_save(int enabled, const uint8_t *in, size_t n, size_t *out_n, PgLog log, void *log_ud);

/* True for the file names the game's save uses (SMR_Save*), so unrelated files are never parsed. */
int postgame_is_save_name(const char *base_name);

#ifdef __cplusplus
}
#endif
#endif
