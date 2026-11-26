// Star Road Recomp options overlay (task-16): options file, applying options, menu input and frame pacing. No ImGui in this file. Our own code.
// Threads: the platform layer (main thread) loads the options at startup, feeds the pads every 5 ms and applies window and keyboard map
// requests; the menu (overlay.cpp) runs on the presenting thread and edits the options under the lock. Everything between them is either
// under a mutex or an atomic.
#include <SDL3/SDL.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

#include "../plat.h"
#include "overlay.h"
#include "overlay_internal.h"
#include "../../gfx/upscale/upscale.h"

namespace {

std::recursive_mutex g_optmx;
Options g_opts;
bool g_loaded = false;
char g_path[MAX_PATH];
unsigned g_saved_rev = 0;
ULONGLONG g_changed_at = 0;
OptFileWatch g_file_watch{};

std::atomic<int> g_open{0};
std::atomic<int> g_fps_hz{0};
std::atomic<int> g_req_window{-1};

std::mutex g_reqmx;
char g_req_keymap[OPT_PATH_MAX];
bool g_req_keymap_pending = false;

std::mutex g_padmx;
ovl::PadSnapshot g_pad{};
uint64_t g_prev_nav = 0, g_ignore = 0, g_sticky = 0;
bool g_sticky_stick = false, g_was_open = false, g_prev_combo = false, g_prev_f1 = false;

std::mutex g_evmx;
std::deque<SDL_Event> g_events;

const int kFps[] = {0, 30, 60, 90, 120, 144, 240};
const float kSharp[] = {-1.0f, 0.0f, 0.1f, 0.25f, 1.0f, 2.0f};     // RCAS attenuation in stops (upscale.h): off, sharpest .. softest
const uint32_t kTarget[][2] = {{0, 0}, {1920, 1080}, {2560, 1440}, {3840, 2160}};

bool EnvSet(const char* n) { const char* v = getenv(n); return v && *v; }

// startup: only values that differ from the default are pushed into the runtime, so the default build path is the one that always ran
void ApplyOne(int id, const Options& o, bool startup) {
  const int v = o.v[id];
  if (startup && opt_is_default(&o, id)) return;
  switch (id) {
    case OPT_WINDOW_MODE: if (!startup) g_req_window = v; break;      // the window itself is touched on the main thread (overlay_main_tick)
    case OPT_UPSCALE: if (!EnvSet("SMRPG_UPSCALE")) upscale::SetMode((upscale::Mode)v); break;
    case OPT_UPSCALE_TARGET: if (!EnvSet("SMRPG_UPSCALE_TARGET")) upscale::SetTarget(kTarget[v][0], kTarget[v][1]); break;
    case OPT_SHARPNESS: if (!EnvSet("SMRPG_UPSCALE_SHARPNESS")) upscale::SetSharpness(kSharp[v]); break;
    case OPT_FPS_CAP: g_fps_hz = kFps[v]; break;
    case OPT_UNLOCK_POSTGAME: break;                                   // read by the game (overlay_unlock_postgame) when a save loads or a new game starts
    case OPT_AUDIO: break;                                             // restart: a null audout sink has no stream to promote to a device
    case OPT_VOLUME: plat_audio_set_volume(v); break;
    case OPT_INPUT_MAP: {
      if (startup) break;                                              // load_keymap reads it at startup
      std::lock_guard<std::mutex> lk(g_reqmx);
      snprintf(g_req_keymap, sizeof g_req_keymap, "%s", o.path[id]);
      g_req_keymap_pending = true;
      break;
    }
  }
}

void LogValues(const char* when) {
  char line[400] = "", t[OPT_PATH_MAX];
  size_t n = 0;
  for (int i = 0; i < OPT_COUNT && n < sizeof line - 80; i++) {
    opt_text(&g_opts, i, t, sizeof t);
    n += (size_t)snprintf(line + n, sizeof line - n, " %s=%s", opt_desc(i)->key, t);
  }
  plat_log("overlay: options (%s):%s", when, line);
}

void ApplyReloaded(int id, void*) {
  ApplyOne(id, g_opts, false);
  char t[OPT_PATH_MAX];
  opt_text(&g_opts, id, t, sizeof t);
  const OptApply apply = opt_desc(id)->apply;
  const char* timing = apply == OPT_RESTART ? " (applies on restart)" : (apply == OPT_ON_LOAD ? " (next save load or new game)" : "");
  plat_log("overlay: applied options.ini change %s.%s = %s%s", opt_desc(id)->section, opt_desc(id)->key, t, timing);
}

}  // namespace

namespace ovl {

void Lock() { g_optmx.lock(); }
void Unlock() { g_optmx.unlock(); }
Options& Opts() { return g_opts; }
const char* FilePath() { return g_path; }
bool IsOpen() { return g_open.load() != 0; }

void ApplyAll() { for (int i = 0; i < OPT_COUNT; i++) ApplyOne(i, g_opts, false); }

void OptionChanged(int id) {
  ApplyOne(id, g_opts, false);
  char t[OPT_PATH_MAX];
  opt_text(&g_opts, id, t, sizeof t);
  plat_log("overlay: option %s.%s = %s%s", opt_desc(id)->section, opt_desc(id)->key, t, opt_needs_restart(&g_opts, id) ? " (takes effect on restart)" : "");
  g_changed_at = GetTickCount64();
}

void SaveIfDue(bool force) {
  if (g_opts.revision == g_saved_rev) return;
  if (!force && GetTickCount64() - g_changed_at < 400) return;
  if (opts_save(&g_opts, g_path) == 0) plat_log("overlay: options saved to %s", g_path);
  else plat_log("overlay: cannot write %s", g_path);
  g_saved_rev = g_opts.revision;                  // a failed write is not retried every frame
}

static void Open(const char* why) {
  if (!g_open.exchange(1)) plat_log("overlay: opened (%s)", why);
}

void Close(const char* why) {
  if (g_open.exchange(0)) plat_log("overlay: closed (%s)", why);
  std::lock_guard<std::recursive_mutex> lk(g_optmx);
  SaveIfDue(true);
}

PadSnapshot ConsumePad() {
  std::lock_guard<std::mutex> lk(g_padmx);
  PadSnapshot s = g_pad;
  g_pad.pressed = 0;
  return s;
}

bool PopEvent(void* out) {
  std::lock_guard<std::mutex> lk(g_evmx);
  if (g_events.empty()) return false;
  *(SDL_Event*)out = g_events.front();
  g_events.pop_front();
  return true;
}

int ListInputMaps(char out[][OPT_PATH_MAX], int max) {
  int n = 0;
  if (max > 0) out[n++][0] = 0;                   // auto
  char exe[MAX_PATH];
  DWORD k = GetModuleFileNameA(nullptr, exe, sizeof exe);
  if (!k || k >= sizeof exe) return n;
  char* slash = strrchr(exe, '\\');
  if (slash) *slash = 0;
  char pat[MAX_PATH + 16];
  snprintf(pat, sizeof pat, "%s\\input*.ini", exe);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return n;
  do {
    if (n < max && strcmp(fd.cFileName, "input.default.ini")) snprintf(out[n++], OPT_PATH_MAX, "%s\\%s", exe, fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return n;
}

}  // namespace ovl

// ---------------------------------------------------------------------------------------------------------- platform layer interface

extern "C" {

void overlay_init(void) {
  std::lock_guard<std::recursive_mutex> lk(g_optmx);
  opts_file_path(g_path, sizeof g_path);
  int n = opts_load(&g_opts, g_path);
  if (n < 0) { opts_defaults(&g_opts); plat_log("overlay: no options file at %s (defaults; the menu creates it on the first change)", g_path); }
  else plat_log("overlay: %d options read from %s", n, g_path);
  g_loaded = true;
  g_saved_rev = g_opts.revision;
  opts_watch_init(&g_file_watch, opts_file_stamp(g_path), GetTickCount64());
  for (int i = 0; i < OPT_COUNT; i++) ApplyOne(i, g_opts, true);
  LogValues("startup");
}

void overlay_shutdown(void) {
  if (!g_loaded) return;
  std::lock_guard<std::recursive_mutex> lk(g_optmx);
  ovl::SaveIfDue(true);
}

int overlay_is_open(void) { return g_open.load(); }

void overlay_toggle(const char* why) {
  if (g_open.load()) { ovl::Close(why ? why : "toggle"); return; }
  ovl::Open(why ? why : "toggle");
}

void overlay_open_from_guest_settings(void) { ovl::Open("game Settings"); }

void overlay_filter_pads(PadState pads[PAD_MAX_PLAYERS], uint32_t* mask) {
  (void)mask;
  PadState& p = pads[0];
  const uint64_t combo = PAD_BIT(PAD_L) | PAD_BIT(PAD_R) | PAD_BIT(PAD_MINUS);
  const bool f1 = (p.buttons & PAD_SCRIPT_OVERLAY) != 0;     // scripted input only: the token F1
  p.buttons &= ~PAD_SCRIPT_OVERLAY;
  const uint64_t raw = p.buttons;
  const bool c = (raw & combo) == combo;
  if (f1 && !g_prev_f1) overlay_toggle("scripted F1");
  if (c && !g_prev_combo) overlay_toggle("controller combo L+R+MINUS");
  g_prev_f1 = f1;
  g_prev_combo = c;

  // the buttons the menu reads: the sixteen pad bits, with the left stick folded onto the d-pad
  uint64_t nav = raw & 0xFFFFull;
  const int32_t kStick = PAD_STICK_MAX / 2;
  if (p.ly > kStick) nav |= PAD_BIT(PAD_UP);
  if (p.ly < -kStick) nav |= PAD_BIT(PAD_DOWN);
  if (p.lx > kStick) nav |= PAD_BIT(PAD_RIGHT);
  if (p.lx < -kStick) nav |= PAD_BIT(PAD_LEFT);

  const bool open = g_open.load() != 0;
  static uint64_t hidden = 0, samples = 0;          // proof for the log: pad samples that carried input while the menu was open
  if (open && !g_was_open) { hidden = 0; samples = 0; }
  if (open) { samples++; if (raw || p.lx || p.ly) hidden++; }
  if (open && !g_was_open) {                       // just opened: what is still held from the opening (the combo) does not press anything
    std::lock_guard<std::mutex> lk(g_padmx);
    g_ignore = nav; g_prev_nav = nav; g_pad = {};
  }
  if (!open && g_was_open)
    plat_log("overlay: while open, %llu pad samples, %llu carried input; all hidden from the game", (unsigned long long)samples, (unsigned long long)hidden);
  if (!open && g_was_open) {                       // just closed: whatever is held now stays hidden from the game until it is let go
    g_sticky = raw;
    g_sticky_stick = p.lx > kStick || p.lx < -kStick || p.ly > kStick || p.ly < -kStick;
  }
  g_was_open = open;

  if (open) {
    std::lock_guard<std::mutex> lk(g_padmx);
    g_ignore &= nav;
    const uint64_t eff = nav & ~g_ignore;
    g_pad.pressed |= eff & ~g_prev_nav;
    g_pad.down = eff;
    g_prev_nav = eff;
    for (int i = 0; i < PAD_MAX_PLAYERS; i++) pads[i] = PadState{};      // the game sees nothing while the menu is open
  } else {
    if (g_sticky) { p.buttons &= ~g_sticky; g_sticky &= raw; }
    if (g_sticky_stick) {
      if (p.lx > kStick / 2 || p.lx < -kStick / 2 || p.ly > kStick / 2 || p.ly < -kStick / 2) { p.lx = 0; p.ly = 0; }
      else g_sticky_stick = false;
    }
  }
}

void overlay_event(const void* e) {
  if (!g_open.load()) return;
  const SDL_Event* ev = (const SDL_Event*)e;
  switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    case SDL_EVENT_WINDOW_FOCUS_LOST: break;
    default: return;
  }
  std::lock_guard<std::mutex> lk(g_evmx);
  if (g_events.size() < 256) g_events.push_back(*ev);
}

void overlay_main_tick(void* win) {
  const ULONGLONG now = GetTickCount64();
  const uint64_t stamp = opts_file_stamp(g_path);
  if (opts_watch_poll(&g_file_watch, stamp, now, 250)) {
    std::lock_guard<std::recursive_mutex> lk(g_optmx);
    Options before = g_opts;
    before.extra = nullptr;
    int invalid = 0;
    const int loaded = opts_reload_live(&g_opts, g_path, &invalid);
    if (loaded < 0) plat_log("overlay: options.ini changed but could not be read: %s", g_path);
    else {
      opts_apply_diff(&before, &g_opts, ApplyReloaded, nullptr);
      g_saved_rev = g_opts.revision;
      plat_log("overlay: options.ini reload complete (%d valid values, %d invalid values)", loaded, invalid);
    }
  }
  int w = g_req_window.exchange(-1);
  if (w >= 0 && win) SDL_SetWindowFullscreen((SDL_Window*)win, w == 1);
  char path[OPT_PATH_MAX];
  bool pending = false;
  {
    std::lock_guard<std::mutex> lk(g_reqmx);
    if (g_req_keymap_pending) { snprintf(path, sizeof path, "%s", g_req_keymap); g_req_keymap_pending = false; pending = true; }
  }
  if (pending) plat_apply_keymap(path);
}

const char* overlay_input_map(void) {
  static char path[OPT_PATH_MAX];
  std::lock_guard<std::recursive_mutex> lk(g_optmx);
  snprintf(path, sizeof path, "%s", g_opts.path[OPT_INPUT_MAP]);
  return path;
}

int overlay_audio_enabled(void) { std::lock_guard<std::recursive_mutex> lk(g_optmx); return g_opts.v[OPT_AUDIO]; }
int overlay_option(int id) {
  std::lock_guard<std::recursive_mutex> lk(g_optmx);
  return (id >= 0 && id < OPT_COUNT) ? g_opts.v[id] : 0;
}
int overlay_unlock_postgame(void) { return overlay_option(OPT_UNLOCK_POSTGAME); }
int overlay_audio_volume(void) { std::lock_guard<std::recursive_mutex> lk(g_optmx); return g_opts.v[OPT_VOLUME]; }
int overlay_start_fullscreen(void) { std::lock_guard<std::recursive_mutex> lk(g_optmx); return g_opts.v[OPT_WINDOW_MODE] == 1; }

void overlay_note_fullscreen(int on) {
  std::lock_guard<std::recursive_mutex> lk(g_optmx);
  if (opt_set(&g_opts, OPT_WINDOW_MODE, on ? 1 : 0)) g_changed_at = GetTickCount64();      // saved by the next menu frame or at exit
}

// The frame rate cap: block the presenting thread until the next frame is due. With "game" (the default) this returns at once.
void overlay_pace(void) {
  static LARGE_INTEGER freq, next;
  static int last_hz = 0;
  const int hz = g_fps_hz.load(std::memory_order_relaxed);
  if (hz <= 0) { last_hz = 0; return; }
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const LONGLONG interval = freq.QuadPart / hz;
  if (hz != last_hz || !next.QuadPart) { last_hz = hz; next.QuadPart = now.QuadPart + interval; return; }   // (re)start the clock
  for (;;) {
    QueryPerformanceCounter(&now);
    const LONGLONG left = next.QuadPart - now.QuadPart;
    if (left <= 0) break;
    const LONGLONG ms = left * 1000 / freq.QuadPart;
    if (ms > 2) Sleep((DWORD)(ms - 1));
    else YieldProcessor();
  }
  next.QuadPart += interval;
  if (now.QuadPart > next.QuadPart + interval * 2) next.QuadPart = now.QuadPart + interval;     // far behind (a stall): do not burst to catch up
  // proof for the log: what the cap really delivered
  static LONGLONG t0 = 0;
  static int n = 0;
  if (!t0) t0 = now.QuadPart;
  if (++n == 240) {
    plat_log("overlay: frame cap %d: 240 frames in %.2f s = %.1f fps", hz, (double)(now.QuadPart - t0) / (double)freq.QuadPart, 240.0 * (double)freq.QuadPart / (double)(now.QuadPart - t0));
    t0 = now.QuadPart; n = 0;
  }
}

}  // extern "C"
