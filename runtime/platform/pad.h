/* Star Road Recomp platform layer (task-22): host input to Switch Pro Controller state. Our own code.
 *
 * Pure mapping, no window and no device needed: SDL events (keyboard, gamepad) go in, one PadState per player comes out. The tests feed it
 * synthetic SDL_Event values (tests/platform/test_platform.c). Switch conventions: stick Y is positive up, range +-32767; the button bit
 * numbers are nn::hid::NpadButton. */
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "padstate.h"

typedef struct PadKeyMap {
    SDL_Scancode button[PAD_NUM_MAPPABLE][2];   /* two keys per button (0 = SDL_SCANCODE_UNKNOWN: unused) */
    SDL_Scancode lstick[4];                      /* up, down, left, right */
    SDL_Scancode rstick[4];
} PadKeyMap;

#define PAD_MAX_GAMEPADS 4
typedef struct PadInput {
    PadKeyMap keymap;
    bool key[SDL_SCANCODE_COUNT];
    struct { SDL_JoystickID id; bool used; uint64_t buttons; int16_t axis[6]; } pad[PAD_MAX_GAMEPADS];   /* index = SDL_GamepadAxis */
} PadInput;

void pad_keymap_defaults(PadKeyMap *m);
/* Reads `[keyboard]` lines `NAME = KEY[, KEY]` from an ini file over the defaults; returns the number of bindings read, -1 if the file is
 * unreadable. NAME: A B X Y LSTICK RSTICK L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN, STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT.
 * KEY: an SDL scancode name ("K", "Return", "Keypad 8", "Left Shift"). */
int pad_keymap_load(PadKeyMap *m, const char *path);
int pad_button_from_name(const char *name);       /* "A".."DOWN" -> PAD_*, -1 unknown */

void pad_input_init(PadInput *in);
void pad_input_clear_keys(PadInput *in);          /* focus lost */
void pad_input_event(PadInput *in, const SDL_Event *e);   /* keyboard keys, gamepad added/removed/buttons/axes; other events ignored */
/* Merges the devices into players: player 0 is the keyboard plus the first gamepad, gamepad n (n >= 1) is player n. connected_mask bit n is
 * set when player n has a device (player 0 always: the virtual Pro Controller). */
void pad_input_compose(const PadInput *in, PadState out[PAD_MAX_PLAYERS], uint32_t *connected_mask);

/* Scripted input: "frame:BUTTON:length,..." (length defaults to 10 frames); frames count presented frames. Buttons: A B X Y L R ZL ZR PLUS
 * MINUS LSTICK RSTICK LEFT UP RIGHT DOWN; left stick STICK_UP/DOWN/LEFT/RIGHT or STICK_<degrees> (0 right, 90 up); right stick RSTICK_*. */
typedef struct PadScriptEntry { uint32_t frame, len; uint64_t buttons; int32_t lx, ly, rx, ry; } PadScriptEntry;
typedef struct PadScript { PadScriptEntry *e; int n; } PadScript;
int pad_script_parse(PadScript *s, const char *text, char *err, int errsz);   /* 0 ok, -1 error text in err */
void pad_script_state(const PadScript *s, uint32_t frame, PadState *out);       /* what is held at `frame` (buttons ORed, sticks summed and clamped) */
void pad_script_free(PadScript *s);
