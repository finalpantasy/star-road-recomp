// Star Road Recomp options overlay (task-16): the menu and its drawing with Dear ImGui (MIT, v1.93.0, fetched by tools/thirdparty/fetch_imgui.py).
// Our own code. PlatOverlayDraw (runtime/gfx/upscale/overlay_hook.h) is the strong definition of the hook QueuePresent calls once per frame.
//
// Closed: PlatOverlayDraw returns on its first line, records nothing and touches no Vulkan object, so the present is what it was without the
// overlay. Open: ImGui draws into the same command buffer on top of the finished picture (dynamic rendering, load op LOAD).
// The menu is driven by the pad snapshot the platform layer collects (overlay_apply.cpp: keyboard, controller or scripted input all arrive as
// Switch buttons), not by ImGui's own navigation, so it behaves the same with any input and is fully reachable with the d-pad, A and B:
//   up/down move, left/right change, A next value (or press), X default, L/R page, B close.
// The mouse also works (click a row = next value, right click = previous) through the SDL3 backend; its events are queued by the main thread
// and handed to ImGui here, because ImGui is only ever used on the presenting thread.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"

#include "../../gfx/upscale/overlay_hook.h"
#include "../padstate.h"
#include "../plat.h"
#include "overlay_internal.h"

namespace {

// ---------------------------------------------------------------------------------------------------------------- state
struct Backend {
  bool ctx = false, sdl = false, vk = false, failed = false;
  void* device = nullptr;
  uint32_t format = 0;
  PFN_vkCmdBeginRendering beginRendering = nullptr;
  PFN_vkCmdEndRendering endRendering = nullptr;
  ImFont* font = nullptr;
  bool srgb = false;
} B;

struct Loader { PFN_vkGetInstanceProcAddr gipa; PFN_vkGetDeviceProcAddr gdpa; VkInstance inst; VkDevice dev; };

PFN_vkVoidFunction LoadVk(const char* name, void* user) {
  auto* L = (Loader*)user;
  PFN_vkVoidFunction f = L->gdpa ? L->gdpa(L->dev, name) : nullptr;
  return f ? f : L->gipa(L->inst, name);
}

// The menu's own cursor state (presenting thread only)
struct Item { int opt; int action; };       // opt >= 0: an option; else action 1 = reset everything, 2 = close
enum { kActReset = 1, kActClose = 2 };
struct Page { const char* name; std::vector<Item> items; };
std::vector<Page> g_pages;
int g_page = 0, g_sel = 0;
bool g_confirm_reset = false, g_was_open = false;
ULONGLONG g_rep_at[4] = {0, 0, 0, 0};       // d-pad auto repeat: up, down, left, right

// ------------------------------------------------------------------------------------------------------------- colours
// The target is B8G8R8A8_SRGB when the game draws sRGB: colours written as plain sRGB numbers would come out too bright, so convert to linear.
ImU32 Col(int r, int g, int b, int a = 255) {
  auto lin = [&](int c) { return B.srgb ? (int)std::lround(std::pow(c / 255.0, 2.2) * 255.0) : c; };
  return IM_COL32(lin(r), lin(g), lin(b), a);
}
const int kYellow[3] = {255, 214, 64};

// -------------------------------------------------------------------------------------------------------------- setup
void BuildPages() {
  g_pages.clear();
  for (int i = 0; i < OPT_COUNT; i++) {
    const char* pg = opt_desc(i)->page;
    if (g_pages.empty() || strcmp(g_pages.back().name, pg)) g_pages.push_back({pg, {}});
    g_pages.back().items.push_back({i, 0});
  }
  g_pages.push_back({"Menu", {{-1, kActReset}, {-1, kActClose}}});
}

bool InitContext() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;     // the game window keeps its own cursor
  const char* root = getenv("SystemRoot");
  const char* names[] = {"\\Fonts\\ARLRDBD.TTF", "\\Fonts\\segoeuib.ttf", "\\Fonts\\arialbd.ttf"};     // a bold rounded UI font if the system has one
  for (const char* n : names) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s%s", root ? root : "C:\\Windows", n);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
    B.font = io.Fonts->AddFontFromFileTTF(path, 22.0f);
    if (B.font) { plat_log("overlay: font %s", path); break; }
  }
  if (!B.font) { B.font = io.Fonts->AddFontDefault(); plat_log("overlay: font: Dear ImGui default"); }
  ImGui::GetStyle().WindowRounding = 0;
  BuildPages();
  B.ctx = true;
  return true;
}

bool InitBackends(const PlatOverlayFrame* f) {
  if (!B.sdl) {
    SDL_Window* win = (SDL_Window*)plat_sdl_window();
    if (!win || !ImGui_ImplSDL3_InitForVulkan(win)) { plat_log("overlay: SDL3 backend failed to start"); return false; }
    B.sdl = true;
  }
  static Loader L;
  L.gipa = (PFN_vkGetInstanceProcAddr)f->get_instance_proc_addr;
  L.inst = (VkInstance)f->instance;
  L.dev = (VkDevice)f->device;
  L.gdpa = (PFN_vkGetDeviceProcAddr)L.gipa(L.inst, "vkGetDeviceProcAddr");
  if (!L.gdpa) return false;
  B.beginRendering = (PFN_vkCmdBeginRendering)L.gdpa(L.dev, "vkCmdBeginRendering");
  B.endRendering = (PFN_vkCmdEndRendering)L.gdpa(L.dev, "vkCmdEndRendering");
  if (!B.beginRendering || !B.endRendering) { plat_log("overlay: vkCmdBeginRendering missing"); return false; }
  if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, LoadVk, &L)) { plat_log("overlay: cannot load the Vulkan functions"); return false; }
  static VkFormat colorFormat;
  colorFormat = (VkFormat)f->format;
  ImGui_ImplVulkan_InitInfo vi = {};
  vi.ApiVersion = VK_API_VERSION_1_3;
  vi.Instance = L.inst;
  vi.PhysicalDevice = (VkPhysicalDevice)f->physical_device;
  vi.Device = L.dev;
  vi.QueueFamily = f->queue_family;
  vi.Queue = (VkQueue)f->queue;
  vi.DescriptorPoolSize = 16;
  vi.MinImageCount = 2;
  vi.ImageCount = std::max(2u, f->image_count);
  vi.UseDynamicRendering = true;
  vi.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  vi.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  vi.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
  if (!ImGui_ImplVulkan_Init(&vi)) { plat_log("overlay: Vulkan backend failed to start"); return false; }
  B.vk = true;
  B.device = f->device;
  B.format = f->format;
  B.srgb = f->format == (uint32_t)VK_FORMAT_B8G8R8A8_SRGB || f->format == (uint32_t)VK_FORMAT_R8G8B8A8_SRGB;
  plat_log("overlay: Dear ImGui %s ready (format %u%s, %ux%u, %u images)", IMGUI_VERSION, f->format, B.srgb ? " sRGB" : "", f->width, f->height, f->image_count);
  return true;
}

// the pipeline is built for one colour format: if the swapchain changes it, start the Vulkan half again
bool EnsureReady(const PlatOverlayFrame* f) {
  if (B.failed) return false;
  if (!B.ctx && !InitContext()) { B.failed = true; return false; }
  if (B.vk && (B.format != f->format || B.device != f->device)) { ImGui_ImplVulkan_Shutdown(); B.vk = false; plat_log("overlay: swapchain format changed, rebuilding the pipeline"); }
  if (!B.vk && !InitBackends(f)) { B.failed = true; return false; }
  return true;
}

// ----------------------------------------------------------------------------------------------------------------- menu
void Select(int page, int sel) {
  g_page = (page % (int)g_pages.size() + (int)g_pages.size()) % (int)g_pages.size();
  const int n = (int)g_pages[g_page].items.size();
  g_sel = (sel % n + n) % n;
  g_confirm_reset = false;
  const Item& it = g_pages[g_page].items[g_sel];
  plat_log("overlay: page %s, row %s", g_pages[g_page].name, it.opt >= 0 ? opt_desc(it.opt)->label : (it.action == kActReset ? "Reset all options" : "Close menu"));
}

// choose the next or previous keyboard map from the files beside the exe
void StepInputMap(Options& o, int dir) {
  static char maps[16][OPT_PATH_MAX];
  const int n = ovl::ListInputMaps(maps, 16);
  int cur = 0;
  for (int i = 0; i < n; i++) if (!_stricmp(maps[i], o.path[OPT_INPUT_MAP])) cur = i;
  const int next = ((cur + dir) % n + n) % n;
  opt_set_path(&o, OPT_INPUT_MAP, maps[next]);
}

void ChangeItem(const Item& it, int dir, bool press) {
  Options& o = ovl::Opts();
  if (it.opt < 0) {
    if (!press) return;
    if (it.action == kActClose) { ovl::Close("menu"); return; }
    if (!g_confirm_reset) { g_confirm_reset = true; plat_log("overlay: reset all options: press A again to confirm"); return; }
    g_confirm_reset = false;
    for (int i = 0; i < OPT_COUNT; i++) { opt_reset(&o, i); ovl::OptionChanged(i); }
    plat_log("overlay: every option reset to its default");
    return;
  }
  const OptDesc* d = opt_desc(it.opt);
  bool changed;
  if (d->type == OPT_PATH) { const unsigned before = o.revision; StepInputMap(o, dir); changed = o.revision != before; }
  else if (d->type == OPT_INT && press && o.v[it.opt] >= d->max) changed = opt_set(&o, it.opt, d->min);      // A at the top wraps to the bottom; the arrows stop
  else changed = opt_step(&o, it.opt, dir) != 0;
  if (changed) ovl::OptionChanged(it.opt);
}

void HandleNav(const ovl::PadSnapshot& s) {
  const ULONGLONG now = GetTickCount64();
  const uint64_t dirs[4] = {PAD_BIT(PAD_UP), PAD_BIT(PAD_DOWN), PAD_BIT(PAD_LEFT), PAD_BIT(PAD_RIGHT)};
  bool fire[4] = {false, false, false, false};
  // holding a direction repeats: up and down always, left and right only on a number row (volume); on/off and named values would just flip back and forth
  const Item& cur = g_pages[g_page].items[std::min(g_sel, (int)g_pages[g_page].items.size() - 1)];
  const bool numberRow = cur.opt >= 0 && opt_desc(cur.opt)->type == OPT_INT;
  auto repeatable = [&](int i) { return i < 2 || numberRow; };
  for (int i = 0; i < 4; i++) {
    if (s.pressed & dirs[i]) { fire[i] = true; g_rep_at[i] = now + 400; }
    else if (s.down & dirs[i]) { if (repeatable(i) && now >= g_rep_at[i]) { fire[i] = true; g_rep_at[i] = now + 80; } }
    else g_rep_at[i] = 0;
  }
  const Page& pg = g_pages[g_page];
  if (s.pressed & PAD_BIT(PAD_B)) { ovl::Close("B"); return; }
  if (s.pressed & PAD_BIT(PAD_L)) { Select(g_page - 1, 0); return; }
  if (s.pressed & PAD_BIT(PAD_R)) { Select(g_page + 1, 0); return; }
  if (fire[0]) Select(g_page, g_sel - 1);
  else if (fire[1]) Select(g_page, g_sel + 1);
  const Item& it = pg.items[std::min(g_sel, (int)pg.items.size() - 1)];
  if (fire[2]) ChangeItem(it, -1, false);
  else if (fire[3]) ChangeItem(it, +1, false);
  if (s.pressed & PAD_BIT(PAD_A)) ChangeItem(it, +1, true);
  if ((s.pressed & PAD_BIT(PAD_X)) && it.opt >= 0 && !opt_is_default(&ovl::Opts(), it.opt)) { opt_reset(&ovl::Opts(), it.opt); ovl::OptionChanged(it.opt); }
}

void Tri(ImDrawList* dl, ImVec2 c, float h, bool right, ImU32 col) {
  const float w = h * 0.6f;
  if (right) dl->AddTriangleFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x - w * 0.5f, c.y + h * 0.5f), ImVec2(c.x + w * 0.5f, c.y), col);
  else dl->AddTriangleFilled(ImVec2(c.x + w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), ImVec2(c.x - w * 0.5f, c.y), col);
}

float TextW(float size, const char* t) { return B.font->CalcTextSizeA(size, FLT_MAX, 0.0f, t).x; }

void DrawMenu(const ImVec2& disp, const Options& o) {
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  bg->AddRectFilled(ImVec2(0, 0), disp, Col(0, 0, 0, 120));                      // dim the game behind the panel
  const float ui = disp.y / 720.0f;
  const float W = 700 * ui, H = 470 * ui, pad = 26 * ui;
  const ImVec2 p0((disp.x - W) * 0.5f, (disp.y - H) * 0.5f), p1(p0.x + W, p0.y + H);
  ImGui::SetNextWindowPos(p0);
  ImGui::SetNextWindowSize(ImVec2(W, H));
  ImGui::Begin("##starroad_options", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = B.font;
  // the panel: deep blue with a white rim, like the game's own menu boxes
  dl->AddRectFilled(p0, p1, Col(18, 28, 92, 242), 12 * ui);
  dl->AddRectFilledMultiColor(ImVec2(p0.x + 3 * ui, p0.y + 3 * ui), ImVec2(p1.x - 3 * ui, p0.y + 84 * ui), Col(52, 82, 190, 120), Col(52, 82, 190, 120), Col(18, 28, 92, 0), Col(18, 28, 92, 0));
  dl->AddRect(p0, p1, Col(255, 255, 255), 12 * ui, 0, 3 * ui);
  dl->AddRect(ImVec2(p0.x + 6 * ui, p0.y + 6 * ui), ImVec2(p1.x - 6 * ui, p1.y - 6 * ui), Col(130, 160, 255, 160), 9 * ui, 0, 1.5f * ui);

  // title and page strip
  dl->AddText(font, 30 * ui, ImVec2(p0.x + pad, p0.y + 16 * ui), Col(255, 255, 255), "STAR ROAD OPTIONS");
  const char* toggle = "F1  or  L + R + MINUS";
  dl->AddText(font, 16 * ui, ImVec2(p1.x - pad - TextW(16 * ui, toggle), p0.y + 26 * ui), Col(170, 190, 255), toggle);
  float x = p0.x + pad + 26 * ui;
  const float ty = p0.y + 66 * ui;
  dl->AddText(font, 16 * ui, ImVec2(p0.x + pad, ty + 3 * ui), Col(170, 190, 255), "L");
  for (int i = 0; i < (int)g_pages.size(); i++) {
    const float w = TextW(22 * ui, g_pages[i].name);
    const bool cur = i == g_page;
    dl->AddText(font, 22 * ui, ImVec2(x, ty), cur ? Col(kYellow[0], kYellow[1], kYellow[2]) : Col(200, 212, 255), g_pages[i].name);
    if (cur) dl->AddRectFilled(ImVec2(x, ty + 28 * ui), ImVec2(x + w, ty + 31 * ui), Col(kYellow[0], kYellow[1], kYellow[2]), 2 * ui);
    ImGui::SetCursorScreenPos(ImVec2(x - 6 * ui, ty - 2 * ui));
    char id[32];
    snprintf(id, sizeof id, "##page%d", i);
    if (ImGui::InvisibleButton(id, ImVec2(w + 12 * ui, 34 * ui))) Select(i, 0);
    x += w + 22 * ui;
  }
  dl->AddText(font, 16 * ui, ImVec2(x - 8 * ui, ty + 3 * ui), Col(170, 190, 255), "R");
  dl->AddLine(ImVec2(p0.x + pad, ty + 40 * ui), ImVec2(p1.x - pad, ty + 40 * ui), Col(130, 160, 255, 200), 1.5f * ui);

  // rows
  const Page& pg = g_pages[g_page];
  const float rowH = 46 * ui, rowW = W - 2 * pad, y0 = p0.y + 122 * ui;
  const char* help = "";
  for (int i = 0; i < (int)pg.items.size(); i++) {
    const Item& it = pg.items[i];
    const ImVec2 r0(p0.x + pad, y0 + i * (rowH + 4 * ui)), r1(r0.x + rowW, r0.y + rowH);
    ImGui::SetCursorScreenPos(r0);
    char id[32];
    snprintf(id, sizeof id, "##row%d", i);
    ImGui::InvisibleButton(id, ImVec2(rowW, rowH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { g_sel = i; ChangeItem(it, +1, true); }
    else if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { g_sel = i; ChangeItem(it, -1, false); }
    const bool sel = i == g_sel;
    if (sel) {
      dl->AddRectFilled(r0, r1, Col(70, 104, 220, 200), 8 * ui);
      dl->AddRect(r0, r1, Col(kYellow[0], kYellow[1], kYellow[2]), 8 * ui, 0, 2.5f * ui);
      Tri(dl, ImVec2(r0.x + 14 * ui, (r0.y + r1.y) * 0.5f), 18 * ui, true, Col(kYellow[0], kYellow[1], kYellow[2]));
    }
    const float ly = r0.y + (rowH - 26 * ui) * 0.5f;
    const ImU32 white = Col(255, 255, 255), dim = Col(170, 190, 255), orange = Col(255, 170, 70);
    if (it.opt >= 0) {
      const OptDesc* d = opt_desc(it.opt);
      dl->AddText(font, 24 * ui, ImVec2(r0.x + 34 * ui, ly), white, d->label);
      if (d->apply != OPT_LIVE) dl->AddText(font, 14 * ui, ImVec2(r0.x + 34 * ui + TextW(24 * ui, d->label) + 10 * ui, ly + 8 * ui), dim, d->apply == OPT_RESTART ? "applies on restart" : "applies on next load");
      char val[OPT_PATH_MAX];
      opt_text(&o, it.opt, val, sizeof val);
      const bool restart = opt_needs_restart(&o, it.opt);
      const bool isdef = opt_is_default(&o, it.opt);
      const float vw = TextW(24 * ui, val);
      const float vx = r1.x - 40 * ui - vw;
      dl->AddText(font, 24 * ui, ImVec2(vx, ly), restart ? orange : (isdef ? Col(220, 230, 255) : Col(kYellow[0], kYellow[1], kYellow[2])), val);
      if (sel) { Tri(dl, ImVec2(vx - 16 * ui, (r0.y + r1.y) * 0.5f), 16 * ui, false, white); Tri(dl, ImVec2(r1.x - 22 * ui, (r0.y + r1.y) * 0.5f), 16 * ui, true, white); }
      if (sel) help = d->help;
    } else if (it.action == kActReset) {
      dl->AddText(font, 24 * ui, ImVec2(r0.x + 34 * ui, ly), g_confirm_reset ? orange : white, g_confirm_reset ? "Press A again: reset every option to its default" : "Reset every option to its default");
      if (sel) help = "Puts every option back to the faithful default (upscaling off, no frame cap, original window). The options file is rewritten.";
    } else {
      dl->AddText(font, 24 * ui, ImVec2(r0.x + 34 * ui, ly), white, "Close menu");
      if (sel) help = "Back to the game. B does the same from anywhere.";
    }
  }

  // help text, restart note, controls
  const float hy = p1.y - 128 * ui;
  dl->AddLine(ImVec2(p0.x + pad, hy), ImVec2(p1.x - pad, hy), Col(130, 160, 255, 120), 1.0f * ui);
  dl->AddText(font, 18 * ui, ImVec2(p0.x + pad, hy + 8 * ui), Col(215, 225, 255), help, nullptr, rowW);
  if (ovl::FilePath()[0]) {
    char tmp[MAX_PATH + 32];
    snprintf(tmp, sizeof tmp, "file: %s", ovl::FilePath());
    dl->AddText(font, 13 * ui, ImVec2(p0.x + pad, p1.y - 62 * ui), Col(130, 150, 220), tmp);
  }
  if (opts_any_restart(&o)) {
    std::string who = "Restart the game to apply: ";
    bool first = true;
    for (int i = 0; i < OPT_COUNT; i++) if (opt_needs_restart(&o, i)) { who += (first ? "" : ", "); who += opt_desc(i)->label; first = false; }
    dl->AddText(font, 16 * ui, ImVec2(p0.x + pad, p1.y - 82 * ui), Col(255, 170, 70), who.c_str());
  }
  const char* hints = "Up / Down  move      Left / Right  change      A  next      X  default      L / R  page      B  close";
  dl->AddText(font, 15 * ui, ImVec2(p0.x + pad, p1.y - 36 * ui), Col(190, 205, 255), hints);
  ImGui::End();
}

void RenderFrame(const PlatOverlayFrame* f) {
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGuiIO& io = ImGui::GetIO();
  // lay the menu out in window units but draw it onto the image, whatever its size (an upscaled target, a different aspect than the window)
  ImVec2 disp = io.DisplaySize;
  if (disp.x < 1 || disp.y < 1) disp = ImVec2((float)f->width, (float)f->height);
  const float sx = f->width / disp.x, sy = f->height / disp.y;
  if (std::fabs(sx - sy) > 0.01f * sy) { disp = ImVec2(f->width / sy, (float)f->height / sy); io.DisplaySize = disp; }
  io.DisplayFramebufferScale = ImVec2(sy, sy);

  ImGui::NewFrame();
  ovl::Lock();
  if (!g_was_open) { g_confirm_reset = false; std::fill(g_rep_at, g_rep_at + 4, 0); Select(g_page, g_sel); }
  g_was_open = true;
  HandleNav(ovl::ConsumePad());
  if (ovl::IsOpen()) DrawMenu(disp, ovl::Opts());
  ovl::SaveIfDue(false);
  ovl::Unlock();
  ImGui::Render();
  if (!ovl::IsOpen()) return;                 // closed by this frame's input: draw nothing, the picture stays what the game made

  ImDrawData* dd = ImGui::GetDrawData();
  VkCommandBuffer cb = (VkCommandBuffer)f->command_buffer;
  VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = (VkImageView)f->image_view;
  att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {f->width, f->height}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &att;
  B.beginRendering(cb, &ri);
  ImGui_ImplVulkan_RenderDrawData(dd, cb);
  B.endRendering(cb);
}

}  // namespace

// ----------------------------------------------------------------------------------------------------------------- hook
extern "C" void PlatOverlayDraw(const PlatOverlayFrame* f) {
  if (!ovl::IsOpen()) {                                     // closed: nothing recorded, nothing touched
    g_was_open = false;
    return;
  }
  if (!f || f->struct_size < offsetof(PlatOverlayFrame, swapchain_generation) + sizeof(uint64_t)) return;
  if (f->layout != (uint32_t)VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) return;
  if (!EnsureReady(f)) { ovl::Close("could not start Dear ImGui"); return; }
  SDL_Event e;
  while (ovl::PopEvent(&e)) ImGui_ImplSDL3_ProcessEvent(&e);
  RenderFrame(f);
}
