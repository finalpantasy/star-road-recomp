/* Star Road Recomp platform layer (task-22): SDL3 window, event loop and input publishing. Our own code. See plat.h and docs/PLATFORM.md. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "../rt_internal.h"
#include <timeapi.h>
#include "overlay/overlay.h"
#include "pad.h"
#include "plat.h"

#define PUBLISH_MS 5                               /* the Switch samples pads at 200 Hz */
#define LIVE_INPUT_POLL_MS 100
#define LIVE_INPUT_MAX_BYTES 65535u

static struct {
    SDL_Window *win;
    int show, fullscreen, ready;
    volatile LONG visible;                         /* read by NVN from the guest threads */
    PadInput in;
    PadScript script;
    int scripted;
    const char *live_input_file;
    BY_HANDLE_FILE_INFORMATION live_input_stamp;
    int live_input_seen;
    uint32_t script_base_frame;
    volatile LONG frame;                           /* presented frames: the clock of scripted input */
    uint64_t published;
} P;

void plat_log(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    rt_log("%s", b);
}

static HWND window_hwnd(void) {
    if (!P.win) return NULL;
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(P.win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

/* the proof for "hidden by default": what Windows itself says about the window */
static void log_window_state(const char *when) {
    HWND h = window_hwnd();
    if (!h) { plat_log("platform: window state (%s): no window", when); return; }
    RECT r = {0};
    GetWindowRect(h, &r);
    plat_log("platform: window state (%s): hwnd=%p IsWindowVisible=%d style=%#lx exstyle=%#lx rect=(%ld,%ld)-(%ld,%ld) sdl_hidden=%d", when, (void *)h,
             IsWindowVisible(h) ? 1 : 0, (unsigned long)GetWindowLongPtrA(h, GWL_STYLE), (unsigned long)GetWindowLongPtrA(h, GWL_EXSTYLE), r.left, r.top, r.right,
             r.bottom, (SDL_GetWindowFlags(P.win) & SDL_WINDOW_HIDDEN) ? 1 : 0);
}

/* chosen: a map picked in the options overlay (empty: none). At startup SMRPG_INPUT_CONFIG wins over it; when the overlay changes it live (use_env 0) it does not. */
static void load_keymap(const char *chosen, int use_env) {
    char path[MAX_PATH] = "";
    const char *cfg = use_env ? getenv("SMRPG_INPUT_CONFIG") : NULL;
    if (cfg && *cfg) snprintf(path, sizeof path, "%s", cfg);
    else if (chosen && *chosen) snprintf(path, sizeof path, "%s", chosen);
    else {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
        if (n && n < sizeof exe) {
            char *slash = strrchr(exe, '\\');
            if (slash) {
                *slash = 0;
                snprintf(path, sizeof path, "%s\\input.ini", exe);
                if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) snprintf(path, sizeof path, "%s\\input.default.ini", exe);   /* the shipped copy of config/input.default.ini */
            }
        }
    }
    if (!*path) { plat_log("platform: keyboard map: built-in defaults"); return; }
    int n = pad_keymap_load(&P.in.keymap, path);
    if (n < 0) plat_log("platform: keyboard map: built-in defaults (no %s)", path);
    else plat_log("platform: keyboard map: %d bindings from %s over the defaults", n, path);
}

void plat_apply_keymap(const char *path) {
    pad_keymap_defaults(&P.in.keymap);
    pad_input_clear_keys(&P.in);
    load_keymap(path, 0);
}

static int load_script(void) {
    const char *txt = getenv("SMRPG_INPUT");
    char *filebuf = NULL;
    const char *file = getenv("SMRPG_INPUT_FILE");
    if (file && *file) {
        FILE *f = fopen(file, "rb");
        if (!f) { plat_log("platform: SMRPG_INPUT_FILE %s cannot be opened", file); return 0; }
        filebuf = (char *)calloc(1, 1 << 16);
        size_t n = fread(filebuf, 1, (1 << 16) - 1, f);
        fclose(f);
        for (size_t i = 0; i < n; i++) if (filebuf[i] == '\n' || filebuf[i] == '\r') filebuf[i] = ',';
        txt = filebuf;
    }
    if (!txt || !*txt) { free(filebuf); return 0; }
    char err[160];
    if (pad_script_parse(&P.script, txt, err, sizeof err)) { plat_log("platform: scripted input rejected: %s", err); free(filebuf); return 0; }
    plat_log("platform: scripted input: %d entries (the keyboard and gamepads are ignored); frame = presented frame", P.script.n);
    free(filebuf);
    return 1;
}

static void reload_live_input(uint32_t frame) {
    HANDLE file = CreateFileA(P.live_input_file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;

    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file, &info) ||
        (P.live_input_seen && info.dwVolumeSerialNumber == P.live_input_stamp.dwVolumeSerialNumber &&
         info.nFileIndexHigh == P.live_input_stamp.nFileIndexHigh && info.nFileIndexLow == P.live_input_stamp.nFileIndexLow &&
         info.nFileSizeHigh == P.live_input_stamp.nFileSizeHigh && info.nFileSizeLow == P.live_input_stamp.nFileSizeLow &&
         CompareFileTime(&info.ftLastWriteTime, &P.live_input_stamp.ftLastWriteTime) == 0)) {
        CloseHandle(file);
        return;
    }

    char text[LIVE_INPUT_MAX_BYTES + 1];
    DWORD n = 0;
    const int within_limit = !info.nFileSizeHigh && info.nFileSizeLow <= LIVE_INPUT_MAX_BYTES;
    const int read_ok = within_limit && ReadFile(file, text, info.nFileSizeLow, &n, NULL) && n == info.nFileSizeLow;
    CloseHandle(file);
    if (within_limit && !read_ok) return;
    P.live_input_stamp = info;
    P.live_input_seen = 1;
    if (!read_ok || memchr(text, 0, n)) {
        plat_log("platform: live input rejected: expected up to %u bytes of text", LIVE_INPUT_MAX_BYTES);
        return;
    }

    for (DWORD i = 0; i < n; i++) if (text[i] == '\n' || text[i] == '\r') text[i] = ',';
    text[n] = '\0';
    PadScript script;
    char err[160] = "out of memory";
    if (pad_script_parse(&script, text, err, sizeof err)) {
        plat_log("platform: live input rejected: %s", err);
        return;
    }
    pad_script_free(&P.script);
    P.script = script;
    P.script_base_frame = frame;
    plat_log("platform: live input accepted: %d entries at frame %u", P.script.n, P.script_base_frame);
}

/* the 5 ms pad tick needs a 1 ms timer: Windows 11 ignores the request for a process with only hidden windows unless the process says it
 * wants timer resolution honoured */
static void fine_timer(void) {
    PROCESS_POWER_THROTTLING_STATE st = {PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
    timeBeginPeriod(1);
}

int plat_init(const PlatOptions *o) {
    fine_timer();
    P.show = o->show_window;
    P.fullscreen = o->fullscreen;
    overlay_init();                                                            /* task-16: options file (docs/PLATFORM.md "Options") */
    pad_input_init(&P.in);
    load_keymap(overlay_input_map(), 1);
    P.scripted = load_script();
    P.live_input_file = getenv("SMRPG_INPUT_LIVE_FILE");
    if (P.live_input_file && *P.live_input_file) {
        P.scripted = 1;
        plat_log("platform: live input file %s (the keyboard and gamepads are ignored)", P.live_input_file);
    } else {
        P.live_input_file = NULL;
    }
    plat_audio_set_enabled(o->audio || overlay_audio_enabled());
    plat_log("platform: audio %s", o->audio ? "ON (--audio): a real device opens when the game opens audout" : "off: null sink, no device");

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_InitFlags flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS;
    if (P.show && !P.scripted) flags |= SDL_INIT_GAMEPAD;                        /* a hidden or scripted run never reads a real controller */
    if (!SDL_Init(flags)) { plat_log("platform: SDL_Init failed: %s", SDL_GetError()); return 1; }
    SDL_WindowFlags wf = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (!P.show) wf |= SDL_WINDOW_HIDDEN;
    P.win = SDL_CreateWindow("Star Road Recomp", 1280, 720, wf);
    if (!P.win) { plat_log("platform: cannot create the window: %s", SDL_GetError()); return 1; }
    if (P.show) {
        if (P.fullscreen || overlay_start_fullscreen()) SDL_SetWindowFullscreen(P.win, true);
        SDL_ShowWindow(P.win);
        SDL_RaiseWindow(P.win);
    }
    InterlockedExchange(&P.visible, P.show ? 1 : 0);
    P.ready = 1;
    plat_log("platform: SDL %d.%d.%d video driver %s, window %s (--window %s)", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION, SDL_GetCurrentVideoDriver(),
             P.show ? "shown" : "hidden", P.show ? "given" : "not given");
    log_window_state("created");
    return 0;
}

int plat_window_visible(void) { return P.visible ? 1 : 0; }
uint32_t plat_frame(void) { return (uint32_t)P.frame; }
void *plat_sdl_window(void) { return P.win; }

void plat_present_tick(void) {
    LONG f = InterlockedIncrement(&P.frame);
    InterlockedIncrement(&g_rt_vframe);                                        /* virtual clock under SMRPG_DETERMINISTIC (task-69) */
    if (rt_deterministic()) {                                                  /* the guest clock is virtual, so its own frame limiter no longer waits: hold presents to 60 per real second (asset loads run in real time) */
        static LARGE_INTEGER qf, next;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!qf.QuadPart) { QueryPerformanceFrequency(&qf); next = now; }
        next.QuadPart += qf.QuadPart / 60;
        if (now.QuadPart > next.QuadPart + qf.QuadPart / 10) next = now;       /* far behind: do not burst to catch up */
        while (now.QuadPart < next.QuadPart) {
            LONGLONG ms = (next.QuadPart - now.QuadPart) * 1000 / qf.QuadPart;
            if (ms > 1) Sleep((DWORD)(ms - 1)); else SwitchToThread();
            QueryPerformanceCounter(&now);
        }
    }
    overlay_pace();                                                            /* frame rate cap (options; "game" = no cap) */
    if (f == 300) log_window_state("present 300");
}

int plat_create_surface(void *vk_instance, uint64_t *vk_surface) {
    VkSurfaceKHR s = 0;
    if (!P.win || !SDL_Vulkan_CreateSurface(P.win, (VkInstance)vk_instance, NULL, &s)) { plat_log("platform: Vulkan surface failed: %s", SDL_GetError()); return 0; }
    *vk_surface = (uint64_t)(uintptr_t)s;
    plat_log("platform: Vulkan surface created from the SDL window (%s)", P.visible ? "visible" : "hidden");
    return 1;
}

void plat_destroy_surface(void *vk_instance, uint64_t vk_surface) {
    SDL_Vulkan_DestroySurface((VkInstance)vk_instance, (VkSurfaceKHR)(uintptr_t)vk_surface, NULL);
}

static void handle_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: rt_log("platform: window closed, exiting"); rt_exit(0, "window closed");
    case SDL_EVENT_WINDOW_SHOWN: InterlockedExchange(&P.visible, 1); break;
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_MINIMIZED: InterlockedExchange(&P.visible, P.show ? 1 : 0); break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!P.scripted) {
            SDL_Gamepad *g = SDL_OpenGamepad(e->gdevice.which);
            plat_log("platform: gamepad added: %s", g ? SDL_GetGamepadName(g) : SDL_GetError());
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED: {
        SDL_Gamepad *g = SDL_GetGamepadFromID(e->gdevice.which);
        if (g) SDL_CloseGamepad(g);
        plat_log("platform: gamepad removed");
        break;
    }
    case SDL_EVENT_KEY_DOWN:
        if (P.show && !e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 || (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & SDL_KMOD_ALT)))) {
            bool fs = !(SDL_GetWindowFlags(P.win) & SDL_WINDOW_FULLSCREEN);
            SDL_SetWindowFullscreen(P.win, fs);
            overlay_note_fullscreen(fs);
        }
        if (!e->key.repeat && e->key.scancode == SDL_SCANCODE_F1) overlay_toggle("F1");      /* the options overlay (task-16) */
        break;
    default: break;
    }
    overlay_event(e);
    if (!P.scripted) pad_input_event(&P.in, e);
}

static void publish_pads(uint32_t frame) {
    PadState pads[PAD_MAX_PLAYERS];
    uint32_t mask = 1;
    if (P.scripted) {
        memset(pads, 0, sizeof pads);
        pad_script_state(&P.script, frame - P.script_base_frame, &pads[0]);
    } else {
        pad_input_compose(&P.in, pads, &mask);
    }
    overlay_filter_pads(pads, &mask);                                          /* task-16: the overlay's combo; no input reaches the game while it is open */
    pads[0].buttons &= ~PAD_SCRIPT_OVERLAY;
    hle_hid_publish(pads, mask);
    P.published++;
}

void plat_run(void *boot_thread) {
    HANDLE bt = (HANDLE)boot_thread;
    uint64_t next = SDL_GetTicks(), last_stat = next, next_live_input = next;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(&e);
        overlay_main_tick(P.win);
        uint64_t now = SDL_GetTicks();
        if (now >= next) {
            const uint32_t frame = plat_frame();
            if (P.live_input_file && now >= next_live_input) {
                reload_live_input(frame);
                next_live_input = now + LIVE_INPUT_POLL_MS;
            }
            publish_pads(frame);
            next += PUBLISH_MS;
            if (now >= next + 100) next = now + PUBLISH_MS;                         /* fell far behind (a stall): do not burst */
        }
        if (now - last_stat >= 10000) {
            last_stat = now;
            plat_log("platform: %llu pad samples published (target 200 per second), frame %lu", (unsigned long long)P.published, (unsigned long)P.frame);
        }
        if (WaitForSingleObject(bt, 1) == WAIT_OBJECT_0) break;
    }
}

void plat_shutdown(void) {
    if (!P.ready) return;
    P.ready = 0;
    overlay_shutdown();
    if (P.win) SDL_DestroyWindow(P.win);
    P.win = NULL;
    pad_script_free(&P.script);
    SDL_Quit();
    timeEndPeriod(1);
}
