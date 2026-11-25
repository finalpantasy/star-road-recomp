/* Star Road Recomp platform layer tests (task-22). Our own code. Build and run with tests/platform/run_platform_tests.py.
 *
 *   1  hid shared memory layout: the writer's output is read back by an independent reader that follows the game SDK's own algorithm for
 *      nn::hid::GetNpadStates (disassembled from the game's sdk: entry stride 0x30, count at lifo+0x18, tail at lifo+0x10, atomic sampling
 *      number before and after the copy, consecutive sampling numbers or retry). A wrong layout makes that reader fail or loop.
 *   2  input mapping with synthetic SDL events (keyboard and gamepad) to the Pro Controller layout
 *   3  keyboard map ini
 *   4  scripted input text
 *   5  audio: muted by default (no SDL audio subsystem, no device); with audio on and SDL_AUDIO_DRIVER=dummy a stream opens and drains
 * Exit 0 all pass. No window, no real device: SDL is initialised with the dummy audio driver only. */
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/platform/hidshm.h"
#include "../../runtime/platform/pad.h"
#include "../../runtime/platform/plat.h"

void plat_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("  [plat] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); g_checks++; if (_a != _b) { g_fail++; printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

/* ------------------------------------------------------------------ 1: hid shared memory */
/* Constants written out from the SDK disassembly, not taken from hidshm.h, so a wrong constant there cannot hide. */
#define SDK_NPAD_BASE 0x9A00u
#define SDK_NPAD_STRIDE 0x5000u
#define SDK_LIFO 0x28u

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static int32_t rd32s(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }

typedef struct { uint64_t sampling, buttons; int32_t lx, ly, rx, ry; uint32_t attr; } SdkState;

/* nn::hid::GetNpadStates(out, n, id) as the game's sdk does it; returns how many entries it read, -1 when it would loop for ever */
static int sdk_get_npad_states(const uint8_t *shm, int npad, SdkState *out, int n) {
    const uint8_t *s = shm + SDK_NPAD_BASE + (size_t)npad * SDK_NPAD_STRIDE;
    for (int attempt = 0; attempt < 8; attempt++) {
        uint64_t count = rd64(s + 0x40), tail = rd64(s + 0x38);
        if (rd64(s + SDK_LIFO + 8) != 17) return -1;
        int32_t cnt = (int32_t)(count & 0x7fffffff);
        if (n < cnt) cnt = n;
        if (cnt < 1) return 0;
        int ok = 1;
        for (int i = 0; i < cnt; i++) {
            uint32_t idx = (uint32_t)((tail + 17u * 4 - (uint32_t)i) % 17u);
            const uint8_t *e = s + 0x48 + (size_t)idx * 0x30;
            uint64_t a1 = rd64(e), samp = rd64(e + 8);
            out[i].sampling = samp; out[i].buttons = rd64(e + 16);
            out[i].lx = rd32s(e + 24); out[i].ly = rd32s(e + 28); out[i].rx = rd32s(e + 32); out[i].ry = rd32s(e + 36);
            out[i].attr = (uint32_t)rd64(e + 40);
            uint64_t a2 = rd64(e);
            if (a1 != a2) { ok = 0; break; }                         /* torn read: retry */
            if (i > 0 && out[i - 1].sampling - out[i].sampling != 1) { ok = 0; break; }       /* the sdk insists on consecutive numbers */
        }
        if (ok) return cnt;
    }
    return -1;
}

static void test_hidshm(void) {
    printf("hid shared memory layout\n");
    uint8_t *shm = (uint8_t *)calloc(1, HIDSHM_SIZE);
    HidShmWriter w;
    hidshm_writer_init(&w);
    PadState pads[PAD_MAX_PLAYERS];
    memset(pads, 0, sizeof pads);
    SdkState out[16];

    /* nothing published: the sdk sees no entries */
    CHECK_EQ(sdk_get_npad_states(shm, 0, out, 16), -1);       /* buffer_count is 0 before the first publish: the sdk would reject it */

    pads[0].buttons = PAD_BIT(PAD_A);
    hidshm_publish(&w, shm, pads, 1u, 1000);
    CHECK_EQ(hidshm_read_style(shm, 0), HIDSHM_STYLE_FULLKEY);
    CHECK_EQ(hidshm_read_style(shm, 1), 0);                    /* player 2 is not connected */
    int n = sdk_get_npad_states(shm, 0, out, 16);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].buttons, PAD_BIT(PAD_A));
    CHECK_EQ(out[0].sampling, 1);
    CHECK(out[0].attr & 1);

    /* 40 more samples: the ring of 17 wraps twice; every read must stay consecutive */
    for (int i = 0; i < 40; i++) {
        pads[0].buttons = (uint64_t)(i & 7) << 4 | PAD_BIT(PAD_B);
        pads[0].lx = i * 100; pads[0].ly = -i * 50; pads[0].rx = 32767; pads[0].ry = -32767;
        hidshm_publish(&w, shm, pads, 1u, 2000 + (uint64_t)i * 5);
        n = sdk_get_npad_states(shm, 0, out, 16);
        CHECK_EQ(n, (i + 2) < 16 ? (i + 2) : 16);
        if (n > 0) {
            CHECK_EQ(out[0].sampling, (uint64_t)i + 2);
            CHECK_EQ(out[0].lx, i * 100);
            CHECK_EQ(out[0].ly, -i * 50);
            CHECK_EQ(out[0].rx, 32767);
            CHECK_EQ(out[0].ry, -32767);
            CHECK_EQ(out[0].buttons, (uint64_t)(i & 7) << 4 | PAD_BIT(PAD_B));
        }
    }
    CHECK_EQ(n, 16);

    /* our own reader agrees with the independent one */
    HidShmSample hs[20];
    CHECK_EQ(hidshm_read_fullkey(shm, 0, hs, 20), 17);          /* the lifo holds 17 entries (the sdk asked for 16) */
    CHECK_EQ(hs[0].sampling, 41);
    CHECK_EQ(hs[0].lx, 3900);

    /* a second player; player 1 unconnected stays style 0 */
    pads[1].buttons = PAD_BIT(PAD_X);
    hidshm_publish(&w, shm, pads, 3u, 9000);
    CHECK_EQ(hidshm_read_style(shm, 1), HIDSHM_STYLE_FULLKEY);
    CHECK_EQ(sdk_get_npad_states(shm, 1, out, 1), 1);
    CHECK_EQ(out[0].buttons, PAD_BIT(PAD_X));
    hidshm_publish(&w, shm, pads, 1u, 9005);
    CHECK_EQ(hidshm_read_style(shm, 1), 0);                      /* unplugged */

    /* a block mapped after earlier writes still gets its static fields on the next publish (the service rewrites every 5 ms) */
    uint8_t *shm2 = (uint8_t *)calloc(1, HIDSHM_SIZE);
    hidshm_publish(&w, shm2, pads, 1u, 9010);
    CHECK_EQ(hidshm_read_style(shm2, 0), HIDSHM_STYLE_FULLKEY);
    CHECK_EQ(rd64(shm2 + SDK_NPAD_BASE + SDK_LIFO + 8), 17);
    free(shm2);
    free(shm);
}

/* ------------------------------------------------------------------ 2: input mapping */
static SDL_Event key_ev(SDL_Scancode sc, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc; e.key.down = down;
    return e;
}
static SDL_Event gbtn_ev(SDL_JoystickID id, SDL_GamepadButton b, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = id; e.gbutton.button = (Uint8)b; e.gbutton.down = down;
    return e;
}
static SDL_Event gaxis_ev(SDL_JoystickID id, SDL_GamepadAxis a, int16_t v) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = id; e.gaxis.axis = (Uint8)a; e.gaxis.value = v;
    return e;
}
static SDL_Event gadd_ev(SDL_JoystickID id, bool add) {
    SDL_Event e;
    SDL_zero(e);
    e.type = add ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
    e.gdevice.which = id;
    return e;
}

static void test_input(void) {
    printf("input mapping (synthetic SDL events)\n");
    PadInput in;
    PadState p[PAD_MAX_PLAYERS];
    uint32_t mask;
    SDL_Event e;
    pad_input_init(&in);

    pad_input_compose(&in, p, &mask);
    CHECK_EQ(mask, 1);                                          /* the keyboard is player 0 and always there */
    CHECK_EQ(p[0].buttons, 0);

    /* keyboard: the diamond is positional (L = Switch A, K = B), W/A/S/D is the left stick */
    e = key_ev(SDL_SCANCODE_L, true); pad_input_event(&in, &e);
    e = key_ev(SDL_SCANCODE_RETURN, true); pad_input_event(&in, &e);
    e = key_ev(SDL_SCANCODE_W, true); pad_input_event(&in, &e);
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(p[0].buttons & PAD_BIT(PAD_A), PAD_BIT(PAD_A));
    CHECK_EQ(p[0].buttons & PAD_BIT(PAD_PLUS), PAD_BIT(PAD_PLUS));
    CHECK_EQ(p[0].ly, PAD_STICK_MAX);                            /* up is positive on the Switch */
    CHECK_EQ(p[0].lx, 0);
    CHECK(p[0].buttons & PAD_BIT(PAD_STICK_L_UP));
    e = key_ev(SDL_SCANCODE_D, true); pad_input_event(&in, &e);  /* diagonal: not faster than a straight line */
    pad_input_compose(&in, p, &mask);
    CHECK(p[0].lx > 22000 && p[0].lx < 23500);
    CHECK(p[0].ly > 22000 && p[0].ly < 23500);
    e = key_ev(SDL_SCANCODE_SPACE, true); pad_input_event(&in, &e);   /* the second key for A */
    e = key_ev(SDL_SCANCODE_L, false); pad_input_event(&in, &e);
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(p[0].buttons & PAD_BIT(PAD_A), PAD_BIT(PAD_A));
    e = key_ev(SDL_SCANCODE_SPACE, false); pad_input_event(&in, &e);
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(p[0].buttons & PAD_BIT(PAD_A), 0);
    e = key_ev(SDL_SCANCODE_UP, true); pad_input_event(&in, &e);
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(p[0].buttons & PAD_BIT(PAD_UP), PAD_BIT(PAD_UP));   /* the arrow keys are the d-pad */
    SDL_Event fl; SDL_zero(fl); fl.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    pad_input_event(&in, &fl);
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(p[0].buttons, 0);                                    /* focus lost: no stuck keys */
    CHECK_EQ(p[0].lx, 0);

    /* gamepad: positional face buttons (east = Switch A), shoulders, start/back, d-pad */
    e = gadd_ev(7, true); pad_input_event(&in, &e);
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(mask, 1);                                           /* the first gamepad joins player 0 (the keyboard player) */
    struct { SDL_GamepadButton sdl; int bit; } map[] = {
        {SDL_GAMEPAD_BUTTON_EAST, PAD_A}, {SDL_GAMEPAD_BUTTON_SOUTH, PAD_B}, {SDL_GAMEPAD_BUTTON_NORTH, PAD_X}, {SDL_GAMEPAD_BUTTON_WEST, PAD_Y},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_R}, {SDL_GAMEPAD_BUTTON_START, PAD_PLUS},
        {SDL_GAMEPAD_BUTTON_BACK, PAD_MINUS}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_STICK_L}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_STICK_R},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        e = gbtn_ev(7, map[i].sdl, true); pad_input_event(&in, &e);
        pad_input_compose(&in, p, &mask);
        CHECK_EQ(p[0].buttons, PAD_BIT(map[i].bit));
        e = gbtn_ev(7, map[i].sdl, false); pad_input_event(&in, &e);
    }
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(p[0].buttons, 0);

    /* triggers are ZL/ZR */
    e = gaxis_ev(7, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 30000); pad_input_event(&in, &e);
    e = gaxis_ev(7, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 2000); pad_input_event(&in, &e);
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(p[0].buttons, PAD_BIT(PAD_ZL));                      /* a light squeeze below the threshold is not a press */

    /* sticks: SDL y grows downward, the Switch's grows upward; dead zone; full deflection reaches the limit */
    e = gaxis_ev(7, SDL_GAMEPAD_AXIS_LEFTX, 1500); pad_input_event(&in, &e);
    e = gaxis_ev(7, SDL_GAMEPAD_AXIS_LEFTY, -1500); pad_input_event(&in, &e);
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(p[0].lx, 0); CHECK_EQ(p[0].ly, 0);                   /* inside the dead zone */
    e = gaxis_ev(7, SDL_GAMEPAD_AXIS_LEFTX, 32767); pad_input_event(&in, &e);
    e = gaxis_ev(7, SDL_GAMEPAD_AXIS_LEFTY, -32768); pad_input_event(&in, &e);
    e = gaxis_ev(7, SDL_GAMEPAD_AXIS_RIGHTY, 32767); pad_input_event(&in, &e);
    pad_input_compose(&in, p, &mask);
    CHECK(p[0].lx > 20000 && p[0].ly > 20000);                    /* up and right */
    CHECK(p[0].lx <= PAD_STICK_MAX && p[0].ly <= PAD_STICK_MAX);
    CHECK(p[0].ry < -20000);                                      /* pushed down */
    CHECK(p[0].buttons & PAD_BIT(PAD_STICK_L_RIGHT));
    CHECK(p[0].buttons & PAD_BIT(PAD_STICK_L_UP));
    CHECK(p[0].buttons & PAD_BIT(PAD_STICK_R_DOWN));
    e = gaxis_ev(7, SDL_GAMEPAD_AXIS_LEFTX, 32767); pad_input_event(&in, &e);
    e = gaxis_ev(7, SDL_GAMEPAD_AXIS_LEFTY, 0); pad_input_event(&in, &e);
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(p[0].lx, PAD_STICK_MAX);                             /* the rim reaches full deflection after the dead zone rescale */
    CHECK_EQ(p[0].ly, 0);

    /* unplugging clears the slot */
    e = gadd_ev(7, false); pad_input_event(&in, &e);
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(mask, 1);
    CHECK_EQ(p[0].buttons, 0);

    /* an unknown button is ignored */
    e = gadd_ev(9, true); pad_input_event(&in, &e);
    e = gbtn_ev(9, SDL_GAMEPAD_BUTTON_GUIDE, true); pad_input_event(&in, &e);
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(p[0].buttons, 0);
    e = gadd_ev(9, false); pad_input_event(&in, &e);

    /* two gamepads: the second one is player 2 */
    e = gadd_ev(11, true); pad_input_event(&in, &e);
    e = gadd_ev(12, true); pad_input_event(&in, &e);
    e = gbtn_ev(11, SDL_GAMEPAD_BUTTON_EAST, true); pad_input_event(&in, &e);
    e = gbtn_ev(12, SDL_GAMEPAD_BUTTON_NORTH, true); pad_input_event(&in, &e);
    pad_input_compose(&in, p, &mask);
    CHECK_EQ(mask, 3);
    CHECK_EQ(p[0].buttons, PAD_BIT(PAD_A));
    CHECK_EQ(p[1].buttons, PAD_BIT(PAD_X));
}

/* ------------------------------------------------------------------ 3: keyboard map */
static void test_keymap(const char *tmpdir, const char *root) {
    printf("keyboard map ini\n");
    char path[512];
    snprintf(path, sizeof path, "%s\\input_test.ini", tmpdir);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    fputs("; comment\n[other]\nA = Z\n[keyboard]\nA = Z, X\nB = C\nSTICK_UP = Up\nPLUS = Return\nBOGUS = Q\nRSTICK_LEFT = Keypad 1\n", f);
    fclose(f);
    PadKeyMap m;
    pad_keymap_defaults(&m);
    CHECK_EQ(m.button[PAD_A][0], SDL_SCANCODE_L);
    int n = pad_keymap_load(&m, path);
    CHECK_EQ(n, 5);                                              /* A, B, STICK_UP, PLUS, RSTICK_LEFT; BOGUS is skipped, [other] is not read */
    CHECK_EQ(m.button[PAD_A][0], SDL_SCANCODE_Z);
    CHECK_EQ(m.button[PAD_A][1], SDL_SCANCODE_X);
    CHECK_EQ(m.button[PAD_B][0], SDL_SCANCODE_C);
    CHECK_EQ(m.button[PAD_X][0], SDL_SCANCODE_I);                 /* untouched entries keep the defaults */
    CHECK_EQ(m.lstick[0], SDL_SCANCODE_UP);
    CHECK_EQ(m.rstick[2], SDL_SCANCODE_KP_1);
    CHECK_EQ(pad_keymap_load(&m, "no_such_file.ini"), -1);
    remove(path);
    if (root) {                                                  /* config/input.default.ini is exactly the built-in defaults */
        snprintf(path, sizeof path, "%s\\config\\input.default.ini", root);
        PadKeyMap d, l;
        pad_keymap_defaults(&d);
        memset(&l, 0, sizeof l);
        CHECK_EQ(pad_keymap_load(&l, path), 24);
        CHECK(memcmp(&d, &l, sizeof d) == 0);
    }
    CHECK_EQ(pad_button_from_name("zl"), PAD_ZL);
    CHECK_EQ(pad_button_from_name("nope"), -1);
}

/* ------------------------------------------------------------------ 4: scripted input */
static void test_script(void) {
    printf("scripted input\n");
    PadScript s;
    PadState p;
    char err[200] = "";
    CHECK_EQ(pad_script_parse(&s, "100:A:30, 200:STICK_UP:40,260:PLUS:5,300:STICK_45:10,310:RSTICK_RIGHT", err, sizeof err), 0);
    CHECK_EQ(s.n, 5);
    pad_script_state(&s, 99, &p);  CHECK_EQ(p.buttons, 0);
    pad_script_state(&s, 100, &p); CHECK_EQ(p.buttons, PAD_BIT(PAD_A));
    pad_script_state(&s, 129, &p); CHECK_EQ(p.buttons, PAD_BIT(PAD_A));
    pad_script_state(&s, 130, &p); CHECK_EQ(p.buttons, 0);
    pad_script_state(&s, 200, &p); CHECK_EQ(p.ly, PAD_STICK_MAX); CHECK_EQ(p.buttons, PAD_BIT(PAD_STICK_L_UP));
    pad_script_state(&s, 262, &p); CHECK_EQ(p.buttons, PAD_BIT(PAD_PLUS));
    pad_script_state(&s, 300, &p); CHECK(p.lx > 23000 && p.lx < 23300 && p.ly > 23000 && p.ly < 23300);     /* 45 degrees */
    pad_script_state(&s, 315, &p); CHECK_EQ(p.rx, PAD_STICK_MAX);                                             /* default length 10 frames */
    pad_script_state(&s, 320, &p); CHECK_EQ(p.rx, 0);
    pad_script_free(&s);
    CHECK_EQ(pad_script_parse(&s, "100:NOPE:5", err, sizeof err), -1);
    CHECK(strstr(err, "NOPE") != NULL);
    CHECK_EQ(pad_script_parse(&s, "abc:A", err, sizeof err), -1);
    CHECK_EQ(pad_script_parse(&s, "100", err, sizeof err), -1);
    /* overlapping entries: buttons are ORed, sticks summed and clamped */
    CHECK_EQ(pad_script_parse(&s, "10:A:20,10:B:20,10:STICK_RIGHT:20,10:STICK_RIGHT:20", err, sizeof err), 0);
    pad_script_state(&s, 15, &p);
    CHECK_EQ(p.buttons & (PAD_BIT(PAD_A) | PAD_BIT(PAD_B)), PAD_BIT(PAD_A) | PAD_BIT(PAD_B));
    CHECK_EQ(p.lx, PAD_STICK_MAX);
    pad_script_free(&s);
    /* the token F1 toggles the options overlay (task-16): its own bit, no Switch button */
    CHECK_EQ(pad_script_parse(&s, "50:F1:3", err, sizeof err), 0);
    pad_script_state(&s, 51, &p);
    CHECK_EQ(p.buttons, PAD_SCRIPT_OVERLAY);
    pad_script_state(&s, 60, &p);
    CHECK_EQ(p.buttons, 0);
    pad_script_free(&s);
}

/* ------------------------------------------------------------------ 5: audio */
static void test_audio(void) {
    printf("audio (muted by default)\n");
    CHECK_EQ(plat_audio_enabled(), 0);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);
    PlatAudioSink *s = plat_audio_open(48000, 6);
    CHECK(s == NULL);                                            /* no --audio: the null sink, no device, no audio subsystem */
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);
    plat_audio_write(NULL, "x", 1);                              /* the null sink swallows everything */
    CHECK_EQ(plat_audio_queued(NULL), 0);

    printf("audio (--audio, dummy driver: no sound)\n");
    const char *drv = getenv("SDL_AUDIO_DRIVER");
    CHECK(drv && !strcmp(drv, "dummy"));                         /* the test must never open a real device */
    if (!drv || strcmp(drv, "dummy")) return;
    plat_audio_set_enabled(1);
    s = plat_audio_open(48000, 6);
    CHECK(s != NULL);
    if (!s) return;
    const char *cur = SDL_GetCurrentAudioDriver();
    CHECK(cur && !strcmp(cur, "dummy"));
    int16_t pcm[48000 / 100 * 6];                                /* 10 ms of six channel silence-ish */
    for (size_t i = 0; i < sizeof pcm / sizeof pcm[0]; i++) pcm[i] = (int16_t)(i % 200);
    for (int i = 0; i < 5; i++) plat_audio_write(s, pcm, (uint32_t)sizeof pcm);
    CHECK(plat_audio_queued(s) > 0 && plat_audio_queued(s) <= 5u * sizeof pcm);
    plat_audio_pause(s, 1);
    uint64_t q0 = plat_audio_queued(s);
    SDL_Delay(100);
    CHECK_EQ(plat_audio_queued(s), q0);                           /* paused: nothing is taken */
    plat_audio_pause(s, 0);
    uint64_t t0 = SDL_GetTicks(), q = plat_audio_queued(s);
    while (q > 0 && SDL_GetTicks() - t0 < 3000) { SDL_Delay(10); q = plat_audio_queued(s); }
    CHECK_EQ(q, 0);                                               /* the device took everything: buffers would be handed back at the device's pace */
    plat_audio_close(s);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);
    plat_audio_set_enabled(0);
}

int main(int argc, char **argv) {
    const char *tmp = argc > 1 ? argv[1] : ".";
    SDL_setenv_unsafe("SDL_AUDIO_DRIVER", getenv("SDL_AUDIO_DRIVER") ? getenv("SDL_AUDIO_DRIVER") : "dummy", 1);
    test_hidshm();
    test_input();
    test_keymap(tmp, argc > 2 ? argv[2] : NULL);
    test_script();
    test_audio();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
