/* Star Road Recomp: NVN host wiring for the game build (task-13 step 2). Our own code (D-008).
 *
 *   - loads the shader cache (own translator only: shaders/cache/v1, every entry is translator shaderxlate 1; reuse-v1 is retired);
 *   - wraps nvnQueuePresentTexture to save chosen presented frames as PNG (captures live outside the repo).
 *
 * Environment:
 *   SMRPG_SHADER_CACHE   cache root (default A:/AI/Recomps/smrpg-work/shaders/cache/v1; "off" skips loading)
 *   SMRPG_CAPTURE_DIR    directory for frame PNGs (unset: no captures)
 *   SMRPG_CAPTURE_FRAMES comma list of 1-based present numbers to save (default "1")
 *   SMRPG_CAPTURE_EVERY  also save every Nth present (default 0 = off)
 */
#include <stdlib.h>
#include <string.h>

#include "rt_internal.h"
#include "nvn/nvn_rt.h"

int nvn_host_cache_load(const char *root);
int nvn_host_save_png(void *window, int index, const char *path, uint32_t *w, uint32_t *h, uint64_t *nonzero);

static HleFn g_orig_present;
static volatile LONG64 g_presents;
static const char *g_capdir;
static unsigned g_frames[64];
static int g_nframes;
static unsigned g_every;


static int want(unsigned n) {
    for (int i = 0; i < g_nframes; i++) if (g_frames[i] == n) return 1;
    return g_every && n % g_every == 0;
}

static void present_hook(Ctx *c) {
    /* nvnQueuePresentTexture(queue, window, index) */
    unsigned n = (unsigned)InterlockedIncrement64(&g_presents);
    if (g_capdir && want(n)) {
        char path[512]; uint32_t w = 0, h = 0; uint64_t nz = 0;
        snprintf(path, sizeof path, "%s/frame%05u.png", g_capdir, n);
        int ok = nvn_host_save_png((void *)(uintptr_t)c->x[1], (int)c->x[2], path, &w, &h, &nz);
        rt_log("capture: present %u -> %s (%s) %ux%u, %llu non-black pixels", n, path, ok ? "saved" : "FAILED", w, h, (unsigned long long)nz);
    }
    g_orig_present(c);
}

void gfxcap_init(void) {
    nvn_rt_register();
    /* the NVN proc table lists nvnBootstrapLoader as a stub; the real one is the HLE in nvn_register.c, so put it back */
    const HleDef *boot = hle_find("nvnBootstrapLoader");
    if (boot) hle_slot("nvnBootstrapLoader", SLOT_HLE, boot->fn, 0)->fn = boot->fn;
    const char *root = getenv("SMRPG_SHADER_CACHE");
    if (!root || !*root) root = "A:/AI/Recomps/smrpg-work/shaders/cache/v1";
    if (strcmp(root, "off") != 0) rt_log("shader cache %s: %s", root, nvn_host_cache_load(root) ? "loaded" : "NOT LOADED");
    g_capdir = getenv("SMRPG_CAPTURE_DIR");
    if (g_capdir && !*g_capdir) g_capdir = NULL;
    if (!g_capdir) return;
    CreateDirectoryA(g_capdir, NULL);
    const char *fr = getenv("SMRPG_CAPTURE_FRAMES");
    if (!fr || !*fr) fr = "1";
    for (const char *p = fr; *p && g_nframes < 64;) {
        g_frames[g_nframes++] = (unsigned)strtoul(p, (char **)&p, 10);
        while (*p == ',' || *p == ' ') p++;
    }
    const char *ev = getenv("SMRPG_CAPTURE_EVERY");
    g_every = ev ? (unsigned)strtoul(ev, NULL, 10) : 0;
    HleSlot *s = hle_slot("nvnQueuePresentTexture", SLOT_HLE, NULL, 0);
    g_orig_present = s->fn;
    s->fn = present_hook;
    rt_log("capture: dir=%s frames=%s every=%u", g_capdir, fr, g_every);
}
