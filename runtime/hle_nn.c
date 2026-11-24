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
        rt_log("%s first call: %s (lr=%#llx x0=%#llx x1=%#llx)", s->kind == SLOT_HLE ? "HLE" : s->kind == SLOT_STUB ? "STUB" : "WRAP", s->name,
               (unsigned long long)c->x[30], (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
        if (s->nn_service) {
            rt_log("FIRST NN SERVICE REQUEST: %s", s->name);
            svc_report_first_service("nn service symbol", s->name);
        }
    } else if (g_trace && s->kind != SLOT_INTERNAL) {
        rt_log("%s call #%lld %s", s->kind == SLOT_HLE ? "HLE" : s->kind == SLOT_STUB ? "STUB" : "WRAP", (long long)n, s->name);
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

/* ------------------------------------------------------------------ C++ exceptions (doc 4.4: unwind by return)
 * Exception object layout: a 0x80-byte hidden header precedes the object:
 *   [0] handler count (negative = rethrown), [1] type_info, [2] destructor. */
#define EXC_HDR 0x80
static inline int64_t *exc_hdr(void *obj) { return (int64_t *)((char *)obj - EXC_HDR); }

HLE_OVR(cxa_alloc, "__cxa_allocate_exception") {
    char *p = calloc(1, EXC_HDR + c->x[0] + 16);
    c->x[0] = (uint64_t)(p + EXC_HDR);
}
HLE_OVR(cxa_free, "__cxa_free_exception") { if (c->x[0]) free((char *)c->x[0] - EXC_HDR); }
HLE_OVR(cxa_throw, "__cxa_throw") {
    void *obj = (void *)c->x[0];
    int64_t *h = exc_hdr(obj);
    h[0] = 0; h[1] = (int64_t)c->x[1]; h[2] = (int64_t)c->x[2];
    c->exc = obj;
    c->exc_type = c->x[1];
    static volatile LONG nthrow;
    if (g_trace || InterlockedIncrement(&nthrow) <= 20) {
        char nm[160];
        uint64_t np = 0;
        SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(c->x[1] + 8), &np, 8, &got);
        rt_log("C++ throw: object=%p type_info=%#llx name=%s lr=%#llx", obj, (unsigned long long)c->x[1], rt_safe_str(np, nm, sizeof nm), (unsigned long long)c->x[30]);
    }
}
HLE_OVR(cxa_begin_catch, "__cxa_begin_catch") {
    void *obj = (void *)c->x[0];
    RtThread *t = c->thr;
    if (!obj) return;
    int64_t *h = exc_hdr(obj);
    h[0] = h[0] < 0 ? -h[0] + 1 : h[0] + 1;
    if (t && (t->ncaught == 0 || t->caught[t->ncaught - 1] != obj) && t->ncaught < 16) t->caught[t->ncaught++] = obj;
    c->exc = 0;
}
HLE_OVR(cxa_end_catch, "__cxa_end_catch") {
    RtThread *t = c->thr;
    if (!t || !t->ncaught) return;
    void *obj = t->caught[t->ncaught - 1];
    int64_t *h = exc_hdr(obj);
    if (h[0] < 0) {
        if (++h[0] == 0) t->ncaught--;
    } else if (--h[0] == 0) {
        t->ncaught--;
        void *keep = c->exc;
        uint64_t keept = c->exc_type, lr = c->x[30];
        if (h[2]) { c->x[0] = (uint64_t)obj; rt_icall(c, (uint64_t)h[2]); }
        c->exc = keep; c->exc_type = keept; c->x[30] = lr;
        free(h);
    }
}
HLE_OVR(cxa_rethrow, "__cxa_rethrow") {
    RtThread *t = c->thr;
    if (!t || !t->ncaught) rt_fatal(c, "__cxa_rethrow with no active exception", 0);
    void *obj = t->caught[t->ncaught - 1];
    int64_t *h = exc_hdr(obj);
    h[0] = -h[0];
    c->exc = obj;
    c->exc_type = (uint64_t)h[1];
}
HLE_OVR(unwind_resume, "_Unwind_Resume") {
    void *obj = (void *)c->x[0];
    c->exc = obj;
    c->exc_type = obj ? (uint64_t)exc_hdr(obj)[1] : 0;
}
HLE_OVR(cxa_get_ptr, "__cxa_get_exception_ptr") { /* x0 stays the object */ }
HLE_OVR(cxa_cur_type, "__cxa_current_exception_type") {
    RtThread *t = c->thr;
    c->x[0] = (t && t->ncaught) ? (uint64_t)exc_hdr(t->caught[t->ncaught - 1])[1] : 0;
}
HLE_OVR(cxa_uncaught, "__cxa_uncaught_exception") { c->x[0] = 0; }
HLE_OVR(cxa_uncaught2, "__cxa_uncaught_exceptions") { c->x[0] = 0; }
HLE_OVR(gxx_personality, "__gxx_personality_v0") { rt_fatal(c, "__gxx_personality_v0 called: the lifter must resolve landing pads at build time", c->x[30]); }
HLE_OVR(std_terminate, "_ZSt9terminatev") { rt_fatal(c, "std::terminate called", c->x[30]); }
HLE_OVR(cxa_call_unexpected, "__cxa_call_unexpected") { rt_fatal(c, "__cxa_call_unexpected", c->x[0]); }

/* ------------------------------------------------------------------ real implementations that are trivial */
/* Add nn:: HLE here as the boot log shows what is needed, for example:
 *   HLE(os_example, "_ZN2nn2os14SomethingEv") { c->x[0] = 0; }
 * Without an entry, a symbol that no module defines gets a logging stub that returns 0. */
