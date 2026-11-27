#pragma once
void decomp_start(void);
void decomp_summary(void);
void decomp_fp_selftest(void);   /* fp_selftest.c: the Rust float layer against the lifted C helpers (task-53) */
void decomp_fp_shadow_enable(void);   /* fp_selftest.c: per-operation live check of the Rust float layer, ab mode (task-54) */
void decomp_fp_shadow_summary(void);
