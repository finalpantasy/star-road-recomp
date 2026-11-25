/* Star Road Recomp optional monster table overlay (task-25). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DATATBL_SCALE_MIN_PERCENT 100
#define DATATBL_SCALE_MAX_PERCENT 400
#define DATATBL_BOSS_IDS_MAX 256

typedef struct DatatblSettings {
    int hard;
    unsigned hp_percent;
    unsigned damage_percent;
    unsigned boss_hp_percent;
    unsigned boss_damage_percent;
    uint32_t boss_ids[DATATBL_BOSS_IDS_MAX];
    size_t boss_id_count;
} DatatblSettings;

typedef struct DatatblReport {
    size_t monster_count;
    size_t boss_count;
    size_t changed_stats;
    size_t saturated_stats;
} DatatblReport;

void datatbl_settings_defaults(DatatblSettings *settings);
/* Missing options.ini means faithful defaults; mode and factors come from the shared options registry. */
int datatbl_settings_load_options(DatatblSettings *settings);
/* Returns a new same-size image, or 0 on malformed input or allocation failure. Caller frees *out. */
int datatbl_transform_monster(const uint8_t *input, size_t input_size, const DatatblSettings *settings,
                              uint8_t **out, size_t *out_size, DatatblReport *report);
/* Returns a transformed same-size cache image only for hard mode and within max_size. */
int datatbl_overlay_prepare_cache(const uint8_t *input, size_t input_size, const DatatblSettings *settings,
                                  size_t max_size, uint8_t **out, size_t *out_size, DatatblReport *report);

#ifdef __cplusplus
}
#endif
