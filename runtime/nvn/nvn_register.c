/* Star Road Recomp: NVN registration with the runtime HLE table (task-12). Our own code (D-008).
 *
 * Linked into smrpg_boot (see docs/NVN.md). Nothing in the rest of runtime/ has to change: the HLE macro registers `nvnBootstrapLoader`
 * at load time, and the first call creates one HLE slot per NVN proc (real implementation or logging stub).
 *
 * Guest flow: nvnBootstrapLoader("nvnDeviceInitialize" | "nvnDeviceGetProcAddress") -> slot address; the guest then asks
 * nvnDeviceGetProcAddress(device, name) for everything else. Slot addresses are synthetic guest addresses; calling one lands in
 * hle_thunk, which runs the proc's Ctx adapter.
 */
#include <string.h>

#include "../rt_internal.h"
#include "nvn_rt.h"

static HleSlot **g_slot;
static size_t g_n;
static volatile LONG g_state;   /* 0 none, 1 building, 2 ready */

void nvn_rt_register(void) {
    if (g_state == 2) return;
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) { while (g_state != 2) Sleep(0); return; }
    size_t n = nvn_proc_count();
    HleSlot **slots = (HleSlot **)calloc(n ? n : 1, sizeof *slots);
    for (size_t i = 0; i < n; i++) {
        void (*thunk)(struct Ctx *) = nvn_proc_thunk(i);
        slots[i] = hle_slot(nvn_proc_name(i), SLOT_HLE, (HleFn)thunk, 0);
    }
    g_slot = slots; g_n = n;
    g_state = 2;
}

/* name -> guest address of the proc's slot (0 if the name is not an NVN proc) */
static void *nvn_resolve(const char *name) {
    nvn_rt_register();
    for (size_t i = 0; i < g_n; i++)
        if (!strcmp(nvn_proc_name(i), name)) return (void *)(uintptr_t)g_slot[i]->addr;
    return 0;
}

/* nvnBootstrapLoader(const char *name): the one import the game links against. */
HLE(nvn_bootstrap_loader, "nvnBootstrapLoader") {
    nvn_rt_register();
    nvn_set_proc_resolver(nvn_resolve);
    const char *name = (const char *)(uintptr_t)c->x[0];     /* guest memory is identity mapped */
    void *r = 0;
    if (name && (!strcmp(name, "nvnDeviceInitialize") || !strcmp(name, "nvnDeviceGetProcAddress"))) r = nvn_resolve(name);
    c->x[0] = (uint64_t)(uintptr_t)r;
}
