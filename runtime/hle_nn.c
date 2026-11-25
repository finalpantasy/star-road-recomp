/* Star Road Recomp HLE of nn:: imports and host replacements (task-5). Our own code.
 *
 * Every HLE, stub and wrapper symbol owns one synthetic guest address in module slot 3. All slots are
 * registered with hle_thunk, which identifies its slot from c->pc (rt_icall and rt_dispatch set it).
 * Add a real implementation with HLE(id, "mangled name") { ...use c->x[0..7], leave results in c->x[0] }.
 * HLE_OVR beats a lifted export of the same name (used for the libc routines the sdk hand-writes). */
#include "rt_internal.h"

/* ------------------------------------------------------------------ definitions table */
static HleDef g_defs[512];
static int g_ndefs;

void hle_add(const char *name, HleFn fn, unsigned flags) {
    if (g_ndefs >= RT_ARRAY_LEN(g_defs)) return;
    g_defs[g_ndefs++] = (HleDef){name, fn, flags};
}

const HleDef *hle_find(const char *name) {
    for (int i = 0; i < g_ndefs; i++)
        if (!strcmp(g_defs[i].name, name)) return &g_defs[i];
    return 0;
}

/* ------------------------------------------------------------------ slots */
#define SLOT_MAX 16384
#define SLOT_FIRST_OFF 0x1000
static HleSlot *g_slots[SLOT_MAX];
static int g_nslots;
static int32_t g_slot_hash[SLOT_MAX * 2];
static CRITICAL_SECTION g_slotcs;
static volatile LONG g_slotcs_init;

static uint32_t sh(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

/* nn::os, util, init, lmem, diag, mem, TargetConfig do not count as service requests */
int hle_is_nn_service(const char *n) {
    if (strncmp(n, "_ZN2nn", 6) == 0) n += 6;
    else if (strncmp(n, "_ZNK2nn", 7) == 0) n += 7;
    else return 0;
    static const char *safe[] = {"2os", "4util", "4init", "4lmem", "4diag", "3mem", "12TargetConfig"};
    for (int i = 0; i < RT_ARRAY_LEN(safe); i++)
        if (strncmp(n, safe[i], strlen(safe[i])) == 0) return 0;
    return 1;
}

HleSlot *hle_slot(const char *name, SlotKind kind, HleFn fn, uint64_t real) {
    if (InterlockedCompareExchange(&g_slotcs_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_slotcs);
        memset(g_slot_hash, 0xFF, sizeof g_slot_hash);
        g_slotcs_init = 2;
    }
    while (g_slotcs_init != 2) Sleep(0);
    EnterCriticalSection(&g_slotcs);
    uint32_t h = sh(name) & (SLOT_MAX * 2 - 1);
    while (g_slot_hash[h] >= 0) {
        HleSlot *s = g_slots[g_slot_hash[h]];
        if (!strcmp(s->name, name)) { LeaveCriticalSection(&g_slotcs); return s; }
        h = (h + 1) & (SLOT_MAX * 2 - 1);
    }
    if (g_nslots >= SLOT_MAX) { LeaveCriticalSection(&g_slotcs); rt_log("FATAL out of HLE slots"); rt_exit(10, "hle slots"); }
    HleSlot *s = calloc(1, sizeof *s);
    s->name = name;
    s->kind = kind;
    s->fn = fn;
    s->real = real;
    s->nn_service = hle_is_nn_service(name);
    s->addr = GUEST_HLE_BASE + SLOT_FIRST_OFF + 4ULL * (uint64_t)g_nslots;
    g_slot_hash[h] = g_nslots;
    g_slots[g_nslots++] = s;
    rt_reg(s->addr, hle_thunk);
    LeaveCriticalSection(&g_slotcs);
    return s;
}

HleSlot *hle_slot_at(uint64_t a) {
    if (a < GUEST_HLE_BASE + SLOT_FIRST_OFF) return 0;
    uint64_t i = (a - GUEST_HLE_BASE - SLOT_FIRST_OFF) >> 2;
    return i < (uint64_t)g_nslots ? g_slots[i] : 0;
}
int hle_nslots(void) { return g_nslots; }
HleSlot *hle_slot_n(int i) { return g_slots[i]; }

void hle_thunk(Ctx *c) {
    HleSlot *s = hle_slot_at(c->pc);
    if (!s) rt_fatal(c, "HLE thunk entered with c->pc not an HLE slot (interpreter or lifted caller must set c->pc)", c->pc);
    LONG64 n = InterlockedIncrement64(&s->count);
    rt_ring_push(s->kind == SLOT_HLE ? "HLE" : s->kind == SLOT_STUB ? "STUB" : s->kind == SLOT_WRAP ? "WRAP" : "INT", s->name, c->x[0], c->x[1], c->x[2]);
    if (n == 1 && s->kind != SLOT_INTERNAL) {
        rt_log("%s first call: %s (lr=%#llx x0=%#llx x1=%#llx real=%#llx)", s->kind == SLOT_HLE ? "HLE" : s->kind == SLOT_STUB ? "STUB" : "WRAP", s->name,
               (unsigned long long)c->x[30], (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)s->real);
        /* wrapped nn:: symbols forward to the lifted sdk code, which talks to our IPC server (runtime/hle); nothing stops here */
    } else if (g_trace && s->kind != SLOT_INTERNAL) {
        rt_log("%s call #%lld %s [x19=%#llx sp=%#llx]", s->kind == SLOT_HLE ? "HLE" : s->kind == SLOT_STUB ? "STUB" : "WRAP", (long long)n, s->name, (unsigned long long)c->x[19], (unsigned long long)c->x[31]);
    }
    if (s->kind == SLOT_WRAP) {                      /* SMRPG_FSLOG=1: every nn::fs path the guest opens (which movie, which bundle) */
        static int fslog = -1;
        if (fslog < 0) fslog = getenv("SMRPG_FSLOG") != NULL;
        if (fslog && s->name && (strstr(s->name, "2fs8OpenFile") || strstr(s->name, "2fs9GetEntryType") || strstr(s->name, "2fs10GetFileSize")) &&
            c->x[1] > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)c->x[1], 8))
            rt_log("fslog %s \"%.160s\"", s->name, (const char *)(uintptr_t)c->x[1]);
    }
    if (s->kind == SLOT_WRAP && s->name && strstr(s->name, "3hid13GetNpadState")) {
        /* task-22 evidence: what the game's own sdk code read from the hid shared memory. GetNpadState(out, id) / GetNpadStates(out, n, id)
         * both start with the newest NpadXState entry: u64 sampling, u64 buttons, s32 lx, ly, rx, ry. Logged when the buttons change, and for the first few calls. */
        uint64_t out = c->x[0];
        rt_icall(c, s->real);
        static struct { HleSlot *s; uint64_t buttons; int calls; } seen[8];
        int k = 0;
        while (k < 8 && seen[k].s && seen[k].s != s) k++;
        if (k < 8 && out > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)out, 0x28)) {
            if (!seen[k].s) { seen[k].s = s; seen[k].buttons = ~0ull; }
            const uint64_t *e = (const uint64_t *)(uintptr_t)out;
            const int32_t *st = (const int32_t *)(uintptr_t)(out + 16);
            seen[k].calls++;
            if (e[1] != seen[k].buttons || seen[k].calls <= 2) {
                rt_log("hid read: %s call #%d newest entry sampling=%llu buttons=%#llx lstick=(%d,%d) rstick=(%d,%d)", s->name, seen[k].calls,
                       (unsigned long long)e[0], (unsigned long long)e[1], st[0], st[1], st[2], st[3]);
                seen[k].buttons = e[1];
            }
        }
        return;
    }
    switch (s->kind) {
    case SLOT_STUB: c->x[0] = 0; break;
    case SLOT_WRAP: rt_icall(c, s->real); break;
    default: s->fn(c); break;
    }
}

/* ------------------------------------------------------------------ libc replacements (guest address == host address) */
HLE_OVR(memcpy, "memcpy") { memcpy((void *)c->x[0], (const void *)c->x[1], c->x[2]); }
HLE_OVR(memmove, "memmove") { memmove((void *)c->x[0], (const void *)c->x[1], c->x[2]); }
HLE_OVR(memset, "memset") { memset((void *)c->x[0], (int)c->x[1], c->x[2]); }
HLE_OVR(memcmp, "memcmp") { c->x[0] = (uint32_t)memcmp((const void *)c->x[0], (const void *)c->x[1], c->x[2]); }
HLE_OVR(memchr, "memchr") { c->x[0] = (uint64_t)memchr((const void *)c->x[0], (int)c->x[1], c->x[2]); }
HLE_OVR(strlen, "strlen") { c->x[0] = strlen((const char *)c->x[0]); }
HLE_OVR(strcmp, "strcmp") { c->x[0] = (uint32_t)strcmp((const char *)c->x[0], (const char *)c->x[1]); }
HLE_OVR(strncmp, "strncmp") { c->x[0] = (uint32_t)strncmp((const char *)c->x[0], (const char *)c->x[1], c->x[2]); }
HLE_OVR(strcpy, "strcpy") { strcpy((char *)c->x[0], (const char *)c->x[1]); }
HLE_OVR(strchr, "strchr") { c->x[0] = (uint64_t)strchr((const char *)c->x[0], (int)c->x[1]); }

/* ------------------------------------------------------------------ real implementations that are trivial */
/* Add nn:: HLE here as the boot log shows what is needed, for example:
 *   HLE(os_example, "_ZN2nn2os14SomethingEv") { c->x[0] = 0; }
 * Without an entry, a symbol that no module defines gets a logging stub that returns 0. */
