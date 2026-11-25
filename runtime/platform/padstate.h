/* Star Road Recomp platform layer (task-22): Switch Pro Controller state, no SDL needed. Our own code.
 * Switch conventions: stick Y is positive up, range +-32767; the button bit numbers are nn::hid::NpadButton. */
#pragma once
#include <stdint.h>

enum {   /* nn::hid::NpadButton bit numbers */
    PAD_A = 0, PAD_B, PAD_X, PAD_Y, PAD_STICK_L, PAD_STICK_R, PAD_L, PAD_R, PAD_ZL, PAD_ZR, PAD_PLUS, PAD_MINUS,
    PAD_LEFT, PAD_UP, PAD_RIGHT, PAD_DOWN,
    PAD_STICK_L_LEFT, PAD_STICK_L_UP, PAD_STICK_L_RIGHT, PAD_STICK_L_DOWN,
    PAD_STICK_R_LEFT, PAD_STICK_R_UP, PAD_STICK_R_RIGHT, PAD_STICK_R_DOWN,
    PAD_NUM_MAPPABLE = 16           /* the sixteen a keyboard map binds; the stick direction bits are derived from the sticks */
};
#define PAD_BIT(n) (1ull << (n))
#define PAD_STICK_MAX 32767
#define PAD_MAX_PLAYERS 8

typedef struct PadState {
    uint64_t buttons;               /* PAD_BIT(PAD_*) */
    int32_t lx, ly, rx, ry;         /* +-32767, y up */
} PadState;

