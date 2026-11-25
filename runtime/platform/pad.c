/* Star Road Recomp platform layer (task-22): host input to Switch Pro Controller state. Our own code. See pad.h. */
#include "pad.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ names */
static const struct { const char *name; int bit; } k_buttons[] = {
    {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y}, {"LSTICK", PAD_STICK_L}, {"RSTICK", PAD_STICK_R}, {"L", PAD_L}, {"R", PAD_R},
    {"ZL", PAD_ZL}, {"ZR", PAD_ZR}, {"PLUS", PAD_PLUS}, {"MINUS", PAD_MINUS}, {"LEFT", PAD_LEFT}, {"UP", PAD_UP}, {"RIGHT", PAD_RIGHT},
    {"DOWN", PAD_DOWN}};

int pad_button_from_name(const char *name) {
    for (size_t i = 0; i < sizeof k_buttons / sizeof k_buttons[0]; i++)
        if (!SDL_strcasecmp(name, k_buttons[i].name)) return k_buttons[i].bit;
    return -1;
}

/* ------------------------------------------------------------------ keyboard map */
void pad_keymap_defaults(PadKeyMap *m) {
    memset(m, 0, sizeof *m);
#define K(b, k0, k1) do { m->button[b][0] = k0; m->button[b][1] = k1; } while (0)
    /* the right hand diamond is positional: L is right (A), K bottom (B), I top (X), J left (Y) */
    K(PAD_A, SDL_SCANCODE_L, SDL_SCANCODE_SPACE);
    K(PAD_B, SDL_SCANCODE_K, SDL_SCANCODE_UNKNOWN);
    K(PAD_X, SDL_SCANCODE_I, SDL_SCANCODE_UNKNOWN);
    K(PAD_Y, SDL_SCANCODE_J, SDL_SCANCODE_UNKNOWN);
    K(PAD_L, SDL_SCANCODE_Q, SDL_SCANCODE_UNKNOWN);
    K(PAD_R, SDL_SCANCODE_E, SDL_SCANCODE_UNKNOWN);
    K(PAD_ZL, SDL_SCANCODE_1, SDL_SCANCODE_UNKNOWN);
    K(PAD_ZR, SDL_SCANCODE_3, SDL_SCANCODE_UNKNOWN);
    K(PAD_STICK_L, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_UNKNOWN);
    K(PAD_STICK_R, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_UNKNOWN);
    K(PAD_PLUS, SDL_SCANCODE_RETURN, SDL_SCANCODE_UNKNOWN);
    K(PAD_MINUS, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_UNKNOWN);
    K(PAD_LEFT, SDL_SCANCODE_LEFT, SDL_SCANCODE_UNKNOWN);
    K(PAD_UP, SDL_SCANCODE_UP, SDL_SCANCODE_UNKNOWN);
    K(PAD_RIGHT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UNKNOWN);
    K(PAD_DOWN, SDL_SCANCODE_DOWN, SDL_SCANCODE_UNKNOWN);
#undef K
    m->lstick[0] = SDL_SCANCODE_W; m->lstick[1] = SDL_SCANCODE_S; m->lstick[2] = SDL_SCANCODE_A; m->lstick[3] = SDL_SCANCODE_D;
    m->rstick[0] = SDL_SCANCODE_KP_8; m->rstick[1] = SDL_SCANCODE_KP_5; m->rstick[2] = SDL_SCANCODE_KP_4; m->rstick[3] = SDL_SCANCODE_KP_6;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static SDL_Scancode *stick_slot(PadKeyMap *m, const char *name) {
    static const char *dirs[4] = {"UP", "DOWN", "LEFT", "RIGHT"};
    for (int d = 0; d < 4; d++) {
        char n[16];
        snprintf(n, sizeof n, "STICK_%s", dirs[d]);
        if (!SDL_strcasecmp(name, n)) return &m->lstick[d];
        snprintf(n, sizeof n, "RSTICK_%s", dirs[d]);
        if (!SDL_strcasecmp(name, n)) return &m->rstick[d];
    }
    return 0;
}

int pad_keymap_load(PadKeyMap *m, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int in_kb = 0, n = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') { in_kb = !SDL_strncasecmp(s, "[keyboard]", 10); continue; }
        if (!in_kb) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *name = trim(s), *val = trim(eq + 1);
        char *comma = strchr(val, ',');
        if (comma) *comma = 0;
        char *k0 = trim(val), *k1 = comma ? trim(comma + 1) : 0;
        SDL_Scancode s0 = *k0 ? SDL_GetScancodeFromName(k0) : SDL_SCANCODE_UNKNOWN;
        SDL_Scancode s1 = k1 && *k1 ? SDL_GetScancodeFromName(k1) : SDL_SCANCODE_UNKNOWN;
        SDL_Scancode *ss = stick_slot(m, name);
        int b = ss ? -1 : pad_button_from_name(name);
        if (ss) { *ss = s0; n++; }
        else if (b >= 0) { m->button[b][0] = s0; m->button[b][1] = s1; n++; }
    }
    fclose(f);
    return n;
}

/* ------------------------------------------------------------------ device state */
void pad_input_init(PadInput *in) {
    memset(in, 0, sizeof *in);
    pad_keymap_defaults(&in->keymap);
}

void pad_input_clear_keys(PadInput *in) { memset(in->key, 0, sizeof in->key); }

static int pad_slot(PadInput *in, SDL_JoystickID id, bool create) {
    int free_i = -1;
    for (int i = 0; i < PAD_MAX_GAMEPADS; i++) {
        if (in->pad[i].used && in->pad[i].id == id) return i;
        if (!in->pad[i].used && free_i < 0) free_i = i;
    }
    if (!create || free_i < 0) return -1;
    memset(&in->pad[free_i], 0, sizeof in->pad[free_i]);
    in->pad[free_i].used = true;
    in->pad[free_i].id = id;
    return free_i;
}

/* SDL gamepad buttons are positional (south/east/west/north): Switch A is east, B south, X north, Y west */
static int map_gamepad_button(int b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return PAD_B;
    case SDL_GAMEPAD_BUTTON_EAST: return PAD_A;
    case SDL_GAMEPAD_BUTTON_WEST: return PAD_Y;
    case SDL_GAMEPAD_BUTTON_NORTH: return PAD_X;
    case SDL_GAMEPAD_BUTTON_BACK: return PAD_MINUS;
    case SDL_GAMEPAD_BUTTON_START: return PAD_PLUS;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return PAD_STICK_L;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return PAD_STICK_R;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return PAD_L;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return PAD_R;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return PAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return PAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return PAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return PAD_RIGHT;
    default: return -1;
    }
}

void pad_input_event(PadInput *in, const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e->key.scancode > 0 && e->key.scancode < SDL_SCANCODE_COUNT) in->key[e->key.scancode] = e->key.down;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: pad_input_clear_keys(in); break;
    case SDL_EVENT_GAMEPAD_ADDED: pad_slot(in, e->gdevice.which, true); break;
    case SDL_EVENT_GAMEPAD_REMOVED: {
        int i = pad_slot(in, e->gdevice.which, false);
        if (i >= 0) memset(&in->pad[i], 0, sizeof in->pad[i]);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int i = pad_slot(in, e->gbutton.which, true), b = map_gamepad_button(e->gbutton.button);
        if (i < 0 || b < 0) break;
        if (e->gbutton.down) in->pad[i].buttons |= PAD_BIT(b);
        else in->pad[i].buttons &= ~PAD_BIT(b);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int i = pad_slot(in, e->gaxis.which, true);
        if (i >= 0 && e->gaxis.axis >= 0 && e->gaxis.axis < 6) in->pad[i].axis[e->gaxis.axis] = e->gaxis.value;
        break;
    }
    default: break;
    }
}

static int32_t clamp_stick(int32_t v) { return v > PAD_STICK_MAX ? PAD_STICK_MAX : v < -PAD_STICK_MAX ? -PAD_STICK_MAX : v; }

/* SDL axes: y grows downward. Radial dead zone of 12 percent, rescaled so the rim still reaches full deflection. */
static void stick_from_sdl(int16_t sx, int16_t sy, int32_t *ox, int32_t *oy) {
    float x = sx / 32767.0f, y = -sy / 32767.0f, m = sqrtf(x * x + y * y);
    const float dz = 0.12f;
    if (m < dz) { *ox = *oy = 0; return; }
    float k = (m > 1.0f ? 1.0f : m);
    k = (k - dz) / (1.0f - dz) / m;
    *ox = clamp_stick((int32_t)lrintf(x * k * PAD_STICK_MAX));
    *oy = clamp_stick((int32_t)lrintf(y * k * PAD_STICK_MAX));
}

static void key_stick(const PadInput *in, const SDL_Scancode k[4], int32_t *x, int32_t *y) {
    int dx = (in->key[k[3]] ? 1 : 0) - (in->key[k[2]] ? 1 : 0), dy = (in->key[k[0]] ? 1 : 0) - (in->key[k[1]] ? 1 : 0);
    if (k[0] == SDL_SCANCODE_UNKNOWN) dy = 0;
    float s = (dx && dy) ? 0.70710678f : 1.0f;                                    /* a diagonal is not faster than a straight line */
    *x = (int32_t)lrintf(dx * s * PAD_STICK_MAX);
    *y = (int32_t)lrintf(dy * s * PAD_STICK_MAX);
}

static void derive_stick_bits(PadState *p) {
    const int32_t t = PAD_STICK_MAX / 2;
    if (p->lx < -t) p->buttons |= PAD_BIT(PAD_STICK_L_LEFT);
    if (p->lx > t) p->buttons |= PAD_BIT(PAD_STICK_L_RIGHT);
    if (p->ly > t) p->buttons |= PAD_BIT(PAD_STICK_L_UP);
    if (p->ly < -t) p->buttons |= PAD_BIT(PAD_STICK_L_DOWN);
    if (p->rx < -t) p->buttons |= PAD_BIT(PAD_STICK_R_LEFT);
    if (p->rx > t) p->buttons |= PAD_BIT(PAD_STICK_R_RIGHT);
    if (p->ry > t) p->buttons |= PAD_BIT(PAD_STICK_R_UP);
    if (p->ry < -t) p->buttons |= PAD_BIT(PAD_STICK_R_DOWN);
}

void pad_input_compose(const PadInput *in, PadState out[PAD_MAX_PLAYERS], uint32_t *connected_mask) {
    memset(out, 0, PAD_MAX_PLAYERS * sizeof out[0]);
    uint32_t mask = 1;                                                            /* player 0: keyboard (always there) */
    for (int b = 0; b < PAD_NUM_MAPPABLE; b++)
        for (int k = 0; k < 2; k++)
            if (in->keymap.button[b][k] != SDL_SCANCODE_UNKNOWN && in->key[in->keymap.button[b][k]]) out[0].buttons |= PAD_BIT(b);
    int32_t lx, ly, rx, ry;
    key_stick(in, in->keymap.lstick, &lx, &ly);
    key_stick(in, in->keymap.rstick, &rx, &ry);
    out[0].lx = lx; out[0].ly = ly; out[0].rx = rx; out[0].ry = ry;
    for (int i = 0; i < PAD_MAX_GAMEPADS; i++) {
        if (!in->pad[i].used) continue;
        PadState *p = &out[i];
        mask |= 1u << i;
        p->buttons |= in->pad[i].buttons;
        if (in->pad[i].axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] > 16000) p->buttons |= PAD_BIT(PAD_ZL);
        if (in->pad[i].axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] > 16000) p->buttons |= PAD_BIT(PAD_ZR);
        int32_t gx, gy;
        stick_from_sdl(in->pad[i].axis[SDL_GAMEPAD_AXIS_LEFTX], in->pad[i].axis[SDL_GAMEPAD_AXIS_LEFTY], &gx, &gy);
        p->lx = clamp_stick(p->lx + gx); p->ly = clamp_stick(p->ly + gy);
        stick_from_sdl(in->pad[i].axis[SDL_GAMEPAD_AXIS_RIGHTX], in->pad[i].axis[SDL_GAMEPAD_AXIS_RIGHTY], &gx, &gy);
        p->rx = clamp_stick(p->rx + gx); p->ry = clamp_stick(p->ry + gy);
    }
    for (int i = 0; i < PAD_MAX_PLAYERS; i++) derive_stick_bits(&out[i]);
    if (connected_mask) *connected_mask = mask;
}

/* ------------------------------------------------------------------ scripted input */
static int script_add(PadScript *s, PadScriptEntry e) {
    PadScriptEntry *n = (PadScriptEntry *)realloc(s->e, (size_t)(s->n + 1) * sizeof *n);
    if (!n) return -1;
    s->e = n;
    s->e[s->n++] = e;
    return 0;
}

static int script_token(const char *tok, PadScriptEntry *e) {
    static const struct { const char *name; int32_t lx, ly, rx, ry; } sticks[] = {
        {"STICK_UP", 0, PAD_STICK_MAX, 0, 0}, {"STICK_DOWN", 0, -PAD_STICK_MAX, 0, 0}, {"STICK_LEFT", -PAD_STICK_MAX, 0, 0, 0}, {"STICK_RIGHT", PAD_STICK_MAX, 0, 0, 0},
        {"RSTICK_UP", 0, 0, 0, PAD_STICK_MAX}, {"RSTICK_DOWN", 0, 0, 0, -PAD_STICK_MAX}, {"RSTICK_LEFT", 0, 0, -PAD_STICK_MAX, 0}, {"RSTICK_RIGHT", 0, 0, PAD_STICK_MAX, 0}};
    int b = pad_button_from_name(tok);
    if (b >= 0) { e->buttons = PAD_BIT(b); return 0; }
    for (size_t i = 0; i < sizeof sticks / sizeof sticks[0]; i++)
        if (!SDL_strcasecmp(tok, sticks[i].name)) { e->lx = sticks[i].lx; e->ly = sticks[i].ly; e->rx = sticks[i].rx; e->ry = sticks[i].ry; return 0; }
    int right = !SDL_strncasecmp(tok, "RSTICK_", 7);
    if (right || !SDL_strncasecmp(tok, "STICK_", 6)) {
        const char *d = tok + (right ? 7 : 6);
        char *end;
        double deg = strtod(d, &end);
        if (end != d && !*end) {
            int32_t x = (int32_t)lrint(cos(deg * 3.14159265358979 / 180.0) * PAD_STICK_MAX), y = (int32_t)lrint(sin(deg * 3.14159265358979 / 180.0) * PAD_STICK_MAX);
            if (right) { e->rx = x; e->ry = y; } else { e->lx = x; e->ly = y; }
            return 0;
        }
    }
    return -1;
}

int pad_script_parse(PadScript *s, const char *text, char *err, int errsz) {
    memset(s, 0, sizeof *s);
    char *copy = _strdup(text), *save = 0;
    if (!copy) return -1;
    for (char *item = strtok_s(copy, ",", &save); item; item = strtok_s(0, ",", &save)) {
        char *p[3] = {0, 0, 0}, *sv2 = 0;
        int np = 0;
        for (char *t = strtok_s(item, ":", &sv2); t && np < 3; t = strtok_s(0, ":", &sv2)) p[np++] = trim(t);
        char *end;
        PadScriptEntry e = {0};
        if (np < 2 || !*p[0]) { snprintf(err, (size_t)errsz, "bad input entry \"%s\" (want frame:BUTTON[:length])", item); free(copy); pad_script_free(s); return -1; }
        unsigned long fr = strtoul(p[0], &end, 10);
        if (*end) { snprintf(err, (size_t)errsz, "bad frame \"%s\"", p[0]); free(copy); pad_script_free(s); return -1; }
        e.frame = (uint32_t)fr;
        e.len = np >= 3 ? (uint32_t)strtoul(p[2], 0, 10) : 10;
        if (script_token(p[1], &e)) { snprintf(err, (size_t)errsz, "unknown button \"%s\"", p[1]); free(copy); pad_script_free(s); return -1; }
        if (script_add(s, e)) { free(copy); pad_script_free(s); return -1; }
    }
    free(copy);
    return 0;
}

void pad_script_state(const PadScript *s, uint32_t frame, PadState *out) {
    memset(out, 0, sizeof *out);
    int64_t lx = 0, ly = 0, rx = 0, ry = 0;
    for (int i = 0; i < s->n; i++) {
        const PadScriptEntry *e = &s->e[i];
        if (frame < e->frame || frame >= e->frame + e->len) continue;
        out->buttons |= e->buttons;
        lx += e->lx; ly += e->ly; rx += e->rx; ry += e->ry;
    }
    out->lx = clamp_stick((int32_t)(lx > 100000 ? 100000 : lx < -100000 ? -100000 : lx));
    out->ly = clamp_stick((int32_t)(ly > 100000 ? 100000 : ly < -100000 ? -100000 : ly));
    out->rx = clamp_stick((int32_t)(rx > 100000 ? 100000 : rx < -100000 ? -100000 : rx));
    out->ry = clamp_stick((int32_t)(ry > 100000 ? 100000 : ry < -100000 ? -100000 : ry));
    derive_stick_bits(out);
}

void pad_script_free(PadScript *s) { free(s->e); s->e = 0; s->n = 0; }
