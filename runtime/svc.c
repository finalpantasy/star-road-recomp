/* Star Road Recomp Horizon kernel HLE (task-5): svc dispatch, handle table, threads, memory, sync, info.
 * Our own code. Small functions, real Horizon semantics for what boot needs; correctness under stress is
 * not a v0 goal (one global kernel lock and one condition variable). */
#include "rt_internal.h"

volatile LONG64 g_svc_count[0x100];
volatile LONG g_first_service;
uint64_t g_main_thread_handle;

#define PSEUDO_THREAD  0xFFFF8000u
#define PSEUDO_PROCESS 0xFFFF8001u
#define WAIT_MASK      0x40000000u
#define PROGRAM_ID     0x0100BC0018138000ULL
#define PROCESS_ID     0x80ULL
#define TOTAL_MEM      0xCD500000ULL
#define SYSRES_SIZE    0x1000000ULL

enum { KO_THREAD = 1, KO_EVENT, KO_SESSION, KO_TRANSFER, KO_SHMEM, KO_RESLIMIT };
struct KObj { int type; int refs; int signaled; RtThread *thr; char name[24]; uint64_t addr, size; };
typedef struct { KObj *obj; int role; } HEnt;   /* role: events 0 = writable, 1 = readable */
static HEnt g_h[4096];
static uint32_t g_hgen = 1;
static CRITICAL_SECTION g_k;
static CONDITION_VARIABLE g_kcv;

/* ---- handles ---- */
static uint32_t h_new(KObj *o, int role) {
    for (uint32_t i = 1; i < 4096; i++) {
        if (!g_h[i].obj) {
            g_h[i].obj = o; g_h[i].role = role; o->refs++;
            uint32_t h = i | ((g_hgen++ & 0x7FFF) << 15);
            return h;
        }
    }
    return 0;
}
static HEnt *h_ent(uint32_t h) {
    uint32_t i = h & 0x7FFF;
    return (i && i < 4096 && g_h[i].obj) ? &g_h[i] : 0;
}
static KObj *h_get(Ctx *c, uint32_t h) {
    if (h == PSEUDO_THREAD) return c->thr ? c->thr->obj : 0;
    HEnt *e = h_ent(h);
    return e ? e->obj : 0;
}
static KObj *kobj_new(int type, const char *name) {
    KObj *o = calloc(1, sizeof *o);
    o->type = type;
    if (name) snprintf(o->name, sizeof o->name, "%s", name);
    return o;
}
static RtThread *thread_of(Ctx *c, uint32_t h) {
    KObj *o = h_get(c, h);
    return (o && o->type == KO_THREAD) ? o->thr : 0;
}

/* ---- thread id map (host thread id -> RtThread) is carried in Ctx::thr ---- */
static volatile LONG g_tid_next = 0x10;

static RtThread *thread_new(const char *name) {
    RtThread *t = calloc(1, sizeof *t);
    t->tid = (uint32_t)InterlockedIncrement(&g_tid_next);
    t->prio = 44; t->core = 0; t->affinity = 0xF;
    snprintf(t->name, sizeof t->name, "%s", name);
    t->obj = kobj_new(KO_THREAD, name);
    t->obj->thr = t;
    t->handle = h_new(t->obj, 0);
    return t;
}

Ctx *rt_ctx_new(RtThread *t) {
    Ctx *c = VirtualAlloc(NULL, (sizeof(Ctx) + 0xFFF) & ~0xFFFULL, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    c->thr = t;
    t->ctx = c;
    t->tlr = rt_stack_alloc(0x1000, 0x1000);        /* Thread Local Region, 0x200 bytes used */
    c->tpidrro_el0 = t->tlr;
    c->fpcr = 0;
    return c;
}

RtThread *svc_main_thread(void) {
    static RtThread *m;
    if (!m) {
        m = thread_new("main");
        m->prio = 44; m->started = 1;
        g_main_thread_handle = m->handle;
    }
    return m;
}

static int win_prio(int p) {
    return p <= 15 ? THREAD_PRIORITY_HIGHEST : p <= 31 ? THREAD_PRIORITY_ABOVE_NORMAL : p <= 47 ? THREAD_PRIORITY_NORMAL : p <= 58 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST;
}

/* ---- svc names ---- */
static const char *g_svc_names[0x80] = {
    [0x01] = "SetHeapSize", [0x02] = "SetMemoryPermission", [0x03] = "SetMemoryAttribute", [0x04] = "MapMemory", [0x05] = "UnmapMemory",
    [0x06] = "QueryMemory", [0x07] = "ExitProcess", [0x08] = "CreateThread", [0x09] = "StartThread", [0x0A] = "ExitThread",
    [0x0B] = "SleepThread", [0x0C] = "GetThreadPriority", [0x0D] = "SetThreadPriority", [0x0E] = "GetThreadCoreMask", [0x0F] = "SetThreadCoreMask",
    [0x10] = "GetCurrentProcessorNumber", [0x11] = "SignalEvent", [0x12] = "ClearEvent", [0x13] = "MapSharedMemory", [0x14] = "UnmapSharedMemory",
    [0x15] = "CreateTransferMemory", [0x16] = "CloseHandle", [0x17] = "ResetSignal", [0x18] = "WaitSynchronization", [0x19] = "CancelSynchronization",
    [0x1A] = "ArbitrateLock", [0x1B] = "ArbitrateUnlock", [0x1C] = "WaitProcessWideKeyAtomic", [0x1D] = "SignalProcessWideKey", [0x1E] = "GetSystemTick",
    [0x1F] = "ConnectToNamedPort", [0x20] = "SendSyncRequestLight", [0x21] = "SendSyncRequest", [0x22] = "SendSyncRequestWithUserBuffer",
    [0x23] = "SendAsyncRequestWithUserBuffer", [0x24] = "GetProcessId", [0x25] = "GetThreadId", [0x26] = "Break", [0x27] = "OutputDebugString",
    [0x28] = "ReturnFromException", [0x29] = "GetInfo", [0x2A] = "FlushEntireDataCache", [0x2B] = "FlushDataCache", [0x2C] = "MapPhysicalMemory",
    [0x2D] = "UnmapPhysicalMemory", [0x2F] = "GetLastThreadInfo", [0x30] = "GetResourceLimitLimitValue", [0x31] = "GetResourceLimitCurrentValue",
    [0x32] = "SetThreadActivity", [0x33] = "GetThreadContext3", [0x34] = "WaitForAddress", [0x35] = "SignalToAddress", [0x36] = "SynchronizePreemptionState",
    [0x37] = "GetResourceLimitPeakValue", [0x40] = "CreateSession", [0x41] = "AcceptSession", [0x43] = "ReplyAndReceive", [0x44] = "ReplyAndReceiveWithUserBuffer",
    [0x45] = "CreateEvent", [0x48] = "MapPhysicalMemoryUnsafe", [0x49] = "UnmapPhysicalMemoryUnsafe", [0x4B] = "SetUnsafeLimit", [0x50] = "CreateCodeMemory",
    [0x51] = "ControlCodeMemory", [0x52] = "SleepSystem", [0x70] = "CreatePort", [0x72] = "ConnectToPort",
};
const char *svc_name(uint32_t imm) { return imm < 0x80 && g_svc_names[imm] ? g_svc_names[imm] : "?"; }

/* ------------------------------------------------------------------ memory */
static uint64_t g_heap_size;

static void svc_set_heap_size(Ctx *c) {
    uint64_t sz = c->x[1];
    if (sz & 0x1FFFFF || sz > GUEST_HEAPRGN_SIZE) { c->x[0] = RES_INVALID_SIZE; return; }
    if (sz > g_heap_size) {
        if (!VirtualAlloc((void *)(GUEST_HEAPRGN_BASE + g_heap_size), sz - g_heap_size, MEM_COMMIT, PAGE_READWRITE)) { c->x[0] = RES_OUT_OF_MEM; return; }
    } else if (sz < g_heap_size) {
        VirtualFree((void *)(GUEST_HEAPRGN_BASE + sz), g_heap_size - sz, MEM_DECOMMIT);
    }
    g_heap_size = sz;
    c->x[0] = 0;
    c->x[1] = GUEST_HEAPRGN_BASE;
}

static void svc_set_memory_permission(Ctx *c) {
    DWORD old, prot = PAGE_READWRITE;
    switch ((uint32_t)c->x[2]) { case 0: prot = PAGE_NOACCESS; break; case 1: prot = PAGE_READONLY; break; default: prot = PAGE_READWRITE; break; }
    c->x[0] = VirtualProtect((void *)c->x[0], c->x[1], prot, &old) ? 0 : RES_INVALID_STATE;
}

static void svc_set_memory_attribute(Ctx *c) { c->x[0] = 0; }

static void svc_map_memory(Ctx *c) {
    uint64_t dst = c->x[0], src = c->x[1], sz = c->x[2];
    DWORD old;
    if ((dst | src | sz) & 0xFFF || !sz) { c->x[0] = RES_INVALID_ADDR; return; }
    if (!VirtualAlloc((void *)dst, sz, MEM_COMMIT, PAGE_READWRITE) && !VirtualAlloc((void *)dst, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) { c->x[0] = RES_INVALID_ADDR; return; }
    memcpy((void *)dst, (void *)src, sz);                 /* copy semantics: see runtime-handover.md */
    VirtualProtect((void *)src, sz, PAGE_NOACCESS, &old);
    c->x[0] = 0;
}

static void svc_unmap_memory(Ctx *c) {
    uint64_t dst = c->x[0], src = c->x[1], sz = c->x[2];
    DWORD old;
    VirtualProtect((void *)src, sz, PAGE_READWRITE, &old);
    memcpy((void *)src, (void *)dst, sz);
    VirtualFree((void *)dst, sz, MEM_DECOMMIT);
    c->x[0] = 0;
}

static void svc_query_memory(Ctx *c) {
    uint64_t out = c->x[0], addr = c->x[2];
    uint64_t base, size;
    uint32_t type = 0, perm = 0;
    if (addr >= GUEST_ASLR_BASE + GUEST_ASLR_SIZE) {
        base = addr & ~0xFFFULL; size = 0 - base; type = 16; perm = 0;
    } else {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)addr, &mi, sizeof mi)) { c->x[0] = RES_INVALID_ADDR; return; }
        base = (uint64_t)mi.BaseAddress; size = mi.RegionSize;
        if (mi.State == MEM_COMMIT) {
            type = 2; perm = mi.Protect == PAGE_NOACCESS ? 0 : mi.Protect == PAGE_READONLY ? 1 : 3;
            const Module *m = ld_module_of(addr);
            if (m) {
                uint64_t off = addr - m->base, lo, hi;
                if (off < m->text_size) { lo = 0; hi = m->text_size; type = 3; perm = 5; }
                else if (off >= m->rodata_off && off < m->rodata_off + m->rodata_size) { lo = m->rodata_off; hi = lo + m->rodata_size; type = 3; perm = 1; }
                else { lo = m->data_off; hi = (m->image_size + 0xFFF) & ~0xFFFULL; type = 4; perm = 3; }
                if (off >= lo) { base = m->base + lo; size = hi - lo; }
            } else if (addr >= GUEST_HEAPRGN_BASE && addr < GUEST_HEAPRGN_BASE + GUEST_HEAPRGN_SIZE) type = 5;
            else if (addr >= GUEST_ALIAS_BASE && addr < GUEST_ALIAS_BASE + GUEST_ALIAS_SIZE) type = 11;
            else if (addr >= GUEST_STACK_BASE) type = 12;
        } else {
            type = 0; perm = 0;
            /* free: extend over the whole span of reserved or free pages with the same state */
            uint64_t end = base + size;
            for (;;) {
                MEMORY_BASIC_INFORMATION n;
                if (end >= GUEST_ASLR_BASE + GUEST_ASLR_SIZE || !VirtualQuery((void *)end, &n, sizeof n) || n.State == MEM_COMMIT) break;
                end = (uint64_t)n.BaseAddress + n.RegionSize;
            }
            size = end - base;
        }
    }
    uint64_t *o = (uint64_t *)out;
    o[0] = base; o[1] = size;
    ((uint32_t *)o)[4] = type; ((uint32_t *)o)[5] = 0; ((uint32_t *)o)[6] = perm; ((uint32_t *)o)[7] = 0; ((uint32_t *)o)[8] = 0; ((uint32_t *)o)[9] = 0;
    c->x[0] = 0;
    c->x[1] = 0;   /* page info */
}

static void svc_map_shared(Ctx *c) { c->x[0] = h_get(c, (uint32_t)c->x[0]) ? 0 : RES_INVALID_HANDLE; }
static void svc_unmap_shared(Ctx *c) { c->x[0] = h_get(c, (uint32_t)c->x[0]) ? 0 : RES_INVALID_HANDLE; }

static void svc_create_transfer(Ctx *c) {
    KObj *o = kobj_new(KO_TRANSFER, "transfer");
    o->addr = c->x[1]; o->size = c->x[2];
    EnterCriticalSection(&g_k);
    uint32_t h = h_new(o, 0);
    LeaveCriticalSection(&g_k);
    c->x[0] = h ? 0 : RES_OUT_OF_MEM;
    c->x[1] = h;
}

static void svc_flush(Ctx *c) { c->x[0] = 0; }

/* ------------------------------------------------------------------ handles, events */
static void svc_close_handle(Ctx *c) {
    uint32_t h = (uint32_t)c->x[0];
    EnterCriticalSection(&g_k);
    HEnt *e = h_ent(h);
    if (!e) { LeaveCriticalSection(&g_k); c->x[0] = RES_INVALID_HANDLE; return; }
    KObj *o = e->obj;
    e->obj = 0;
    if (--o->refs <= 0 && o->type != KO_THREAD) free(o);
    LeaveCriticalSection(&g_k);
    c->x[0] = 0;
}

static void svc_create_event(Ctx *c) {
    KObj *o = kobj_new(KO_EVENT, "event");
    EnterCriticalSection(&g_k);
    uint32_t w = h_new(o, 0), r = h_new(o, 1);
    LeaveCriticalSection(&g_k);
    c->x[0] = (w && r) ? 0 : RES_OUT_OF_MEM;
    c->x[1] = w;
    c->x[2] = r;
}

static void evt_set(Ctx *c, int v, int need_role) {
    EnterCriticalSection(&g_k);
    HEnt *e = h_ent((uint32_t)c->x[0]);
    if (!e) c->x[0] = RES_INVALID_HANDLE;
    else if (e->obj->type != KO_EVENT) c->x[0] = RES_INVALID_HANDLE;
    else {
        (void)need_role;
        if (!v && !e->obj->signaled && need_role == 1) c->x[0] = RES_INVALID_STATE;
        else { e->obj->signaled = v; c->x[0] = 0; WakeAllConditionVariable(&g_kcv); }
    }
    LeaveCriticalSection(&g_k);
}
static void svc_signal_event(Ctx *c) { evt_set(c, 1, 0); }
static void svc_clear_event(Ctx *c) { evt_set(c, 0, 0); }
static void svc_reset_signal(Ctx *c) { evt_set(c, 0, 1); }

static int obj_signaled(KObj *o) {
    if (o->type == KO_THREAD) return o->thr && o->thr->exited;
    if (o->type == KO_EVENT) return o->signaled;
    return 0;
}

static void svc_wait_sync(Ctx *c) {
    uint32_t *hs = (uint32_t *)c->x[1];
    int32_t n = (int32_t)c->x[2];
    int64_t to = (int64_t)c->x[3];
    if (n < 0 || n > 0x40) { c->x[0] = RES_INVALID_SIZE; c->x[1] = (uint64_t)-1; return; }
    DWORD deadline = to < 0 ? INFINITE : (DWORD)((to + 999999) / 1000000);
    ULONGLONG t0 = GetTickCount64();
    EnterCriticalSection(&g_k);
    for (;;) {
        if (c->thr->cancel_sync) { c->thr->cancel_sync = 0; c->x[0] = RES_CANCELLED; c->x[1] = (uint64_t)-1; break; }
        int found = -1, bad = 0;
        for (int i = 0; i < n; i++) {
            KObj *o = h_get(c, hs[i]);
            if (!o) { bad = 1; break; }
            if (found < 0 && obj_signaled(o)) found = i;
        }
        if (bad) { c->x[0] = RES_INVALID_HANDLE; c->x[1] = (uint64_t)-1; break; }
        if (found >= 0) { c->x[0] = 0; c->x[1] = (uint64_t)found; break; }
        DWORD wait = INFINITE;
        if (deadline != INFINITE) {
            ULONGLONG el = GetTickCount64() - t0;
            if (el >= deadline) { c->x[0] = RES_TIMEOUT; c->x[1] = (uint64_t)-1; break; }
            wait = (DWORD)(deadline - el);
        }
        SleepConditionVariableCS(&g_kcv, &g_k, wait);
    }
    LeaveCriticalSection(&g_k);
}

static void svc_cancel_sync(Ctx *c) {
    RtThread *t = thread_of(c, (uint32_t)c->x[0]);
    if (!t) { c->x[0] = RES_INVALID_HANDLE; return; }
    EnterCriticalSection(&g_k);
    t->cancel_sync = 1;
    WakeAllConditionVariable(&g_kcv);
    LeaveCriticalSection(&g_k);
    c->x[0] = 0;
}

/* ------------------------------------------------------------------ threads */
static DWORD WINAPI guest_thread_main(LPVOID p) {
    RtThread *t = p;
    Ctx *c = rt_ctx_new(t);
    tl_ctx = c;
    (void)rt_tid();
    t->stack_base = t->stack_top;
    c->x[0] = t->arg;
    c->x[31] = t->stack_top;
    SetThreadPriority(GetCurrentThread(), win_prio(t->prio));
    rt_log("guest thread %u '%s' starts: entry=%#llx arg=%#llx sp=%#llx prio=%d tlr=%#llx", t->tid, t->name, (unsigned long long)t->entry,
           (unsigned long long)t->arg, (unsigned long long)t->stack_top, t->prio, (unsigned long long)t->tlr);
    rt_call_guest(c, t->entry);
    rt_log("guest thread %u '%s' returned from its entry", t->tid, t->name);
    EnterCriticalSection(&g_k);
    t->exited = 1;
    WakeAllConditionVariable(&g_kcv);
    LeaveCriticalSection(&g_k);
    return 0;
}

static void svc_create_thread(Ctx *c) {
    uint64_t entry = c->x[1], arg = c->x[2], top = c->x[3];
    int32_t prio = (int32_t)c->x[4], core = (int32_t)c->x[5];
    if (prio < 0 || prio > 63) { c->x[0] = RES_INVALID_PRIO; return; }
    EnterCriticalSection(&g_k);
    RtThread *t = thread_new("guest");
    LeaveCriticalSection(&g_k);
    t->entry = entry; t->arg = arg; t->stack_top = top; t->prio = prio; t->core = core;
    c->x[0] = t->handle ? 0 : RES_OUT_OF_MEM;
    c->x[1] = t->handle;
}

static void svc_start_thread(Ctx *c) {
    RtThread *t = thread_of(c, (uint32_t)c->x[0]);
    if (!t) { c->x[0] = RES_INVALID_HANDLE; return; }
    if (InterlockedExchange(&t->started, 1)) { c->x[0] = RES_INVALID_STATE; return; }
    DWORD id;
    t->host = CreateThread(NULL, 64u << 20, guest_thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, &id);
    c->x[0] = t->host ? 0 : RES_OUT_OF_MEM;
}

static void svc_exit_thread(Ctx *c) {
    RtThread *t = c->thr;
    rt_log("guest thread %u exits via svcExitThread", t->tid);
    EnterCriticalSection(&g_k);
    t->exited = 1;
    WakeAllConditionVariable(&g_kcv);
    LeaveCriticalSection(&g_k);
    ExitThread(0);
}

static void svc_sleep_thread(Ctx *c) {
    int64_t ns = (int64_t)c->x[0];
    if (ns <= 0) SwitchToThread();
    else Sleep((DWORD)((ns + 999999) / 1000000));
}

static void svc_get_thread_priority(Ctx *c) {
    RtThread *t = thread_of(c, (uint32_t)c->x[1]);
    if (!t) { c->x[0] = RES_INVALID_HANDLE; return; }
    c->x[0] = 0; c->x[1] = (uint32_t)t->prio;
}
static void svc_set_thread_priority(Ctx *c) {
    RtThread *t = thread_of(c, (uint32_t)c->x[0]);
    int32_t p = (int32_t)c->x[1];
    if (!t) { c->x[0] = RES_INVALID_HANDLE; return; }
    if (p < 0 || p > 63) { c->x[0] = RES_INVALID_PRIO; return; }
    t->prio = p;
    if (t->host) SetThreadPriority(t->host, win_prio(p));
    c->x[0] = 0;
}
static void svc_get_core_mask(Ctx *c) {
    RtThread *t = thread_of(c, (uint32_t)c->x[2]);
    if (!t) { c->x[0] = RES_INVALID_HANDLE; return; }
    c->x[0] = 0; c->x[1] = (uint32_t)t->core; c->x[2] = t->affinity;
}
static void svc_set_core_mask(Ctx *c) {
    RtThread *t = thread_of(c, (uint32_t)c->x[0]);
    if (!t) { c->x[0] = RES_INVALID_HANDLE; return; }
    t->core = (int32_t)c->x[1]; t->affinity = c->x[2];
    c->x[0] = 0;
}
static void svc_cur_cpu(Ctx *c) { c->x[0] = (uint32_t)(c->thr ? (c->thr->core >= 0 ? c->thr->core : 0) : 0); }
static void svc_set_activity(Ctx *c) { c->x[0] = thread_of(c, (uint32_t)c->x[0]) ? 0 : RES_INVALID_HANDLE; }

static void svc_get_thread_context3(Ctx *c) {
    RtThread *t = thread_of(c, (uint32_t)c->x[1]);
    uint64_t *o = (uint64_t *)c->x[0];
    if (!t) { c->x[0] = RES_INVALID_HANDLE; return; }
    memset(o, 0, 0x320);
    Ctx *s = t->ctx;
    if (s) {
        for (int i = 0; i < 29; i++) o[i] = s->x[i];
        o[29] = s->x[29]; o[30] = s->x[30]; o[31] = s->x[31]; o[32] = s->pc;
        memcpy((char *)o + 0x110, s->v, 0x200);
        ((uint32_t *)((char *)o + 0x310))[0] = s->fpcr;
        ((uint32_t *)((char *)o + 0x310))[1] = s->fpsr;
        o[0x318 / 8] = s->tpidr_el0;
    }
    c->x[0] = 0;
}

static void svc_get_process_id(Ctx *c) { c->x[0] = 0; c->x[1] = PROCESS_ID; }
static void svc_get_thread_id(Ctx *c) {
    RtThread *t = thread_of(c, (uint32_t)c->x[1]);
    if (!t) { c->x[0] = RES_INVALID_HANDLE; return; }
    c->x[0] = 0; c->x[1] = t->tid;
}
static void svc_get_system_tick(Ctx *c) { c->x[0] = rt_cntvct(); }
static void svc_sync_preempt(Ctx *c) { (void)c; }

/* ------------------------------------------------------------------ address arbiter (doc 4.2) */
typedef struct { int used, kind; uint64_t addr, key; uint32_t tag; int64_t seq; int woken; } Waiter;   /* kind: 0 lock, 1 condvar, 2 address */
static Waiter g_w[1024];
static int64_t g_wseq;

static Waiter *w_add(int kind, uint64_t addr, uint64_t key, uint32_t tag) {
    for (int i = 0; i < 1024; i++)
        if (!g_w[i].used) { g_w[i] = (Waiter){1, kind, addr, key, tag, ++g_wseq, 0}; return &g_w[i]; }
    return 0;
}
static Waiter *w_first(int kind, uint64_t addr, uint64_t key, int *count) {
    Waiter *best = 0;
    int n = 0;
    for (int i = 0; i < 1024; i++) {
        Waiter *w = &g_w[i];
        if (!w->used || w->woken || w->kind != kind) continue;
        if ((kind == 1 ? w->key != key : w->addr != addr)) continue;
        n++;
        if (!best || w->seq < best->seq) best = w;
    }
    if (count) *count = n;
    return best;
}
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_ACQUIRE); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_RELEASE); }

/* kernel-side unlock of the Horizon mutex word at addr (g_k held) */
static void mutex_unlock_locked(uint64_t addr) {
    int n;
    Waiter *w = w_first(0, addr, 0, &n);
    if (w) {
        st32(addr, w->tag | (n > 1 ? WAIT_MASK : 0));
        w->woken = 1;
        WakeAllConditionVariable(&g_kcv);
    } else {
        st32(addr, 0);
    }
}

/* wait until a lock waiter entry is granted (g_k held) */
static void lock_wait_locked(Waiter *w) {
    while (!w->woken) SleepConditionVariableCS(&g_kcv, &g_k, INFINITE);
    w->used = 0;
}

static void svc_arbitrate_lock(Ctx *c) {
    uint32_t owner = (uint32_t)c->x[0], tag = (uint32_t)c->x[2];
    uint64_t addr = c->x[1];
    EnterCriticalSection(&g_k);
    if (ld32(addr) != (owner | WAIT_MASK)) { LeaveCriticalSection(&g_k); c->x[0] = 0; return; }
    Waiter *w = w_add(0, addr, 0, tag);
    if (!w) { LeaveCriticalSection(&g_k); c->x[0] = RES_OUT_OF_MEM; return; }
    lock_wait_locked(w);
    LeaveCriticalSection(&g_k);
    c->x[0] = 0;
}

static void svc_arbitrate_unlock(Ctx *c) {
    EnterCriticalSection(&g_k);
    mutex_unlock_locked(c->x[0]);
    LeaveCriticalSection(&g_k);
    c->x[0] = 0;
}

static void svc_wait_pwk(Ctx *c) {
    uint64_t addr = c->x[0], key = c->x[1];
    uint32_t tag = (uint32_t)c->x[2];
    int64_t to = (int64_t)c->x[3];
    DWORD deadline = to < 0 ? INFINITE : (DWORD)((to + 999999) / 1000000);
    ULONGLONG t0 = GetTickCount64();
    int timed_out = 0;
    EnterCriticalSection(&g_k);
    mutex_unlock_locked(addr);
    Waiter *w = w_add(1, addr, key, tag);
    if (!w) { LeaveCriticalSection(&g_k); c->x[0] = RES_OUT_OF_MEM; return; }
    while (!w->woken) {
        DWORD wait = INFINITE;
        if (deadline != INFINITE) {
            ULONGLONG el = GetTickCount64() - t0;
            if (el >= deadline) { timed_out = 1; break; }
            wait = (DWORD)(deadline - el);
        }
        SleepConditionVariableCS(&g_kcv, &g_k, wait);
    }
    w->used = 0;
    /* reacquire the mutex like the kernel does after a wakeup */
    uint32_t v = ld32(addr);
    if (v == 0) {
        st32(addr, tag);
    } else {
        __atomic_fetch_or((uint32_t *)addr, WAIT_MASK, __ATOMIC_ACQ_REL);
        Waiter *lw = w_add(0, addr, 0, tag);
        if (lw) lock_wait_locked(lw);
    }
    LeaveCriticalSection(&g_k);
    c->x[0] = timed_out ? RES_TIMEOUT : 0;
}

static void svc_signal_pwk(Ctx *c) {
    uint64_t key = c->x[0];
    int32_t count = (int32_t)c->x[1];
    EnterCriticalSection(&g_k);
    for (int k = 0; count <= 0 || k < count; k++) {
        Waiter *w = w_first(1, 0, key, 0);
        if (!w) break;
        w->woken = 1;
    }
    WakeAllConditionVariable(&g_kcv);
    LeaveCriticalSection(&g_k);
    c->x[0] = 0;
}

static void svc_wait_for_address(Ctx *c) {
    uint64_t addr = c->x[0];
    uint32_t type = (uint32_t)c->x[1];
    int32_t value = (int32_t)c->x[2];
    int64_t to = (int64_t)c->x[3];
    DWORD deadline = to < 0 ? INFINITE : (DWORD)((to + 999999) / 1000000);
    ULONGLONG t0 = GetTickCount64();
    EnterCriticalSection(&g_k);
    int32_t cur = (int32_t)ld32(addr);
    int ok = 0;
    if (type == 0) ok = cur < value;
    else if (type == 1) { ok = cur < value; if (ok) st32(addr, (uint32_t)(cur - 1)); }
    else if (type == 2) ok = cur == value;
    else { LeaveCriticalSection(&g_k); c->x[0] = RES_INVALID_ENUM; return; }
    if (!ok) { LeaveCriticalSection(&g_k); c->x[0] = RES_INVALID_STATE; return; }
    if (to == 0) { LeaveCriticalSection(&g_k); c->x[0] = RES_TIMEOUT; return; }
    Waiter *w = w_add(2, addr, 0, 0);
    if (!w) { LeaveCriticalSection(&g_k); c->x[0] = RES_OUT_OF_MEM; return; }
    int timed_out = 0;
    while (!w->woken) {
        DWORD wait = INFINITE;
        if (deadline != INFINITE) {
            ULONGLONG el = GetTickCount64() - t0;
            if (el >= deadline) { timed_out = 1; break; }
            wait = (DWORD)(deadline - el);
        }
        SleepConditionVariableCS(&g_kcv, &g_k, wait);
    }
    w->used = 0;
    LeaveCriticalSection(&g_k);
    c->x[0] = timed_out ? RES_TIMEOUT : 0;
}

static void svc_signal_to_address(Ctx *c) {
    uint64_t addr = c->x[0];
    uint32_t type = (uint32_t)c->x[1];
    int32_t value = (int32_t)c->x[2], count = (int32_t)c->x[3];
    EnterCriticalSection(&g_k);
    int n = 0;
    w_first(2, addr, 0, &n);
    if (type == 1 || type == 2) {
        if ((int32_t)ld32(addr) != value) { LeaveCriticalSection(&g_k); c->x[0] = RES_INVALID_STATE; return; }
        int32_t nv = value;
        if (type == 1) nv = value + 1;
        else if (count <= 0) nv = n == 0 ? value - 1 : value + 1;
        else if (n == 0) nv = value + 1;
        else if (count < n) nv = value;
        else nv = value - 1;
        st32(addr, (uint32_t)nv);
    } else if (type != 0) { LeaveCriticalSection(&g_k); c->x[0] = RES_INVALID_ENUM; return; }
    for (int k = 0; count <= 0 || k < count; k++) {
        Waiter *w = w_first(2, addr, 0, 0);
        if (!w) break;
        w->woken = 1;
    }
    WakeAllConditionVariable(&g_kcv);
    LeaveCriticalSection(&g_k);
    c->x[0] = 0;
}

/* ------------------------------------------------------------------ misc */
static const char *break_name(uint32_t r) {
    static const char *n[] = {"Panic", "Assert", "User", "PreLoadDll", "PostLoadDll", "PreUnloadDll", "PostUnloadDll", "CppException"};
    return (r & 0x7FFFFFFF) < 8 ? n[r & 0x7FFFFFFF] : "?";
}

static void svc_break(Ctx *c) {
    uint32_t reason = (uint32_t)c->x[0];
    rt_log("svcBreak reason=%#x (%s)%s info=%#llx size=%#llx lr=%#llx", reason, break_name(reason), (reason & 0x80000000u) ? " [notification only]" : "",
           (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[30]);
    if (reason & 0x80000000u) return;
    rt_fatal(c, "guest called svcBreak (assert, panic or uncaught exception)", reason);
}

static void svc_output_debug(Ctx *c) {
    char buf[600];
    uint64_t n = c->x[1] > 512 ? 512 : c->x[1];
    SIZE_T got = 0;
    if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)c->x[0], buf, n, &got)) {
        buf[got] = 0;
        for (size_t i = 0; i < got; i++) if (buf[i] == '\n' || buf[i] == '\r') buf[i] = ' ';
        rt_log("guest debug output: %s", buf);
    }
    c->x[0] = 0;
}

static void svc_exit_process(Ctx *c) {
    rt_log("svcExitProcess code=%#llx lr=%#llx", (unsigned long long)c->x[0], (unsigned long long)c->x[30]);
    rt_exit(g_first_service ? 0 : 6, "guest called svcExitProcess");
}

static void svc_return_from_exception(Ctx *c) { rt_fatal(c, "svcReturnFromException (guest exception handler returned)", c->x[0]); }

static uint64_t g_reslimit_handle;
static void svc_get_info(Ctx *c) {
    uint64_t type = c->x[1], sub = c->x[3], v = 0;
    uint32_t res = 0;
    switch (type) {
    case 0: v = 0xF; break;                                  /* CoreMask */
    case 1: v = ~0ULL; break;                                /* PriorityMask */
    case 2: v = GUEST_ALIAS_BASE; break;
    case 3: v = GUEST_ALIAS_SIZE; break;
    case 4: v = GUEST_HEAPRGN_BASE; break;
    case 5: v = GUEST_HEAPRGN_SIZE; break;
    case 6: v = TOTAL_MEM; break;
    case 7: v = g_heap_size + g_mod[0].image_size + g_mod[1].image_size + g_mod[2].image_size + SYSRES_SIZE; break;
    case 8: v = 0; break;                                    /* DebuggerAttached */
    case 9:                                                  /* ResourceLimit */
        if (!g_reslimit_handle) {
            KObj *o = kobj_new(KO_RESLIMIT, "reslimit");
            EnterCriticalSection(&g_k);
            g_reslimit_handle = h_new(o, 0);
            LeaveCriticalSection(&g_k);
        }
        v = g_reslimit_handle; break;
    case 10: v = 0; break;                                   /* IdleTickCount */
    case 11: {                                               /* RandomEntropy: fixed for determinism */
        static const uint64_t ent[4] = {0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL, 0x94D049BB133111EBULL, 0xD6E8FEB86659FD93ULL};
        if (sub > 3) { res = RES_INVALID_ENUM; break; }
        v = ent[sub]; break;
    }
    case 12: v = GUEST_ASLR_BASE; break;
    case 13: v = GUEST_ASLR_SIZE; break;
    case 14: v = GUEST_STACK_BASE; break;
    case 15: v = GUEST_STACKRGN_SIZE; break;
    case 16: v = SYSRES_SIZE; break;
    case 17: v = 0; break;
    case 18: v = PROGRAM_ID; break;
    case 20: v = 0; break;                                   /* UserExceptionContextAddress: none registered */
    case 21: v = TOTAL_MEM - SYSRES_SIZE; break;
    case 22: v = g_heap_size + g_mod[0].image_size + g_mod[1].image_size + g_mod[2].image_size; break;
    case 23: v = 1; break;                                   /* IsApplication */
    case 0xF0000002ULL: {                                    /* ThreadTickCount */
        RtThread *t = thread_of(c, (uint32_t)c->x[2]);
        v = t ? rt_cntvct() : 0;
        break;
    }
    default: res = RES_INVALID_ENUM; break;
    }
    c->x[0] = res;
    c->x[1] = res ? 0 : v;
}

static void svc_reslimit(Ctx *c) {
    /* resource: 0 memory, 1 threads, 2 events, 3 transfer memories, 4 sessions */
    uint32_t res = (uint32_t)c->x[2];
    static const uint64_t lim[5] = {TOTAL_MEM, 0x200, 0x2A0, 0x80, 0x384};
    if (res > 4) { c->x[0] = RES_INVALID_ENUM; return; }
    c->x[0] = 0;
    c->x[1] = lim[res];
}
static void svc_reslimit_cur(Ctx *c) {
    uint32_t res = (uint32_t)c->x[2];
    static const uint64_t cur[5] = {0x60000000ULL, 0x20, 0x40, 0, 0x10};
    if (res > 4) { c->x[0] = RES_INVALID_ENUM; return; }
    c->x[0] = 0; c->x[1] = cur[res];
}

/* ------------------------------------------------------------------ IPC (nn service requests) */
static void svc_connect_named_port(Ctx *c) {
    char nm[24];
    rt_safe_str(c->x[1], nm, 13);
    KObj *o = kobj_new(KO_SESSION, nm);
    EnterCriticalSection(&g_k);
    uint32_t h = h_new(o, 0);
    LeaveCriticalSection(&g_k);
    rt_log("FIRST NN SERVICE REQUEST candidate: svcConnectToNamedPort(\"%s\") -> handle %#x lr=%#llx", nm, h, (unsigned long long)c->x[30]);
    c->x[0] = 0;
    c->x[1] = h;
    svc_report_first_service("svcConnectToNamedPort", nm);
}

static void ipc_describe(Ctx *c, char *out, size_t n) {
    uint32_t *m = (uint32_t *)c->tpidrro_el0;
    uint32_t w0 = m[0], w1 = m[1];
    uint32_t type = w0 & 0xFFFF;
    uint32_t cmd = 0xFFFFFFFFu;
    for (int i = 0; i < 0x40; i++)
        if (m[i] == 0x49434653u) { cmd = m[i + 2]; break; }   /* 'SFCI' then version, command id */
    snprintf(out, n, "msgtype=%u hdr=%08x %08x cmif_cmd=%d", type, w0, w1, (int)cmd);
}

static void svc_send_sync_request(Ctx *c) {
    uint32_t h = (uint32_t)c->x[0];
    KObj *o = h_get(c, h);
    char d[160], what[200];
    ipc_describe(c, d, sizeof d);
    snprintf(what, sizeof what, "session '%s' handle %#x %s", o ? o->name : "?", h, d);
    rt_log("FIRST NN SERVICE REQUEST candidate: svcSendSyncRequest(%s) lr=%#llx", what, (unsigned long long)c->x[30]);
    c->x[0] = o ? 0xF601 : RES_INVALID_HANDLE;    /* SessionClosed: we do not emulate services */
    svc_report_first_service("svcSendSyncRequest", what);
}

void svc_report_first_service(const char *what, const char *detail) {
    if (InterlockedCompareExchange(&g_first_service, 1, 0) == 0) rt_boot_reached(what, detail);
}

/* ------------------------------------------------------------------ dispatch */
typedef void (*SvcFn)(Ctx *);
static SvcFn g_svc[0x100];

void svc_init(void) {
    InitializeCriticalSection(&g_k);
    InitializeConditionVariable(&g_kcv);
#define S(n, f) g_svc[n] = f
    S(0x01, svc_set_heap_size); S(0x02, svc_set_memory_permission); S(0x03, svc_set_memory_attribute); S(0x04, svc_map_memory);
    S(0x05, svc_unmap_memory); S(0x06, svc_query_memory); S(0x07, svc_exit_process); S(0x08, svc_create_thread); S(0x09, svc_start_thread);
    S(0x0A, svc_exit_thread); S(0x0B, svc_sleep_thread); S(0x0C, svc_get_thread_priority); S(0x0D, svc_set_thread_priority);
    S(0x0E, svc_get_core_mask); S(0x0F, svc_set_core_mask); S(0x10, svc_cur_cpu); S(0x11, svc_signal_event); S(0x12, svc_clear_event);
    S(0x13, svc_map_shared); S(0x14, svc_unmap_shared); S(0x15, svc_create_transfer); S(0x16, svc_close_handle); S(0x17, svc_reset_signal);
    S(0x18, svc_wait_sync); S(0x19, svc_cancel_sync); S(0x1A, svc_arbitrate_lock); S(0x1B, svc_arbitrate_unlock); S(0x1C, svc_wait_pwk);
    S(0x1D, svc_signal_pwk); S(0x1E, svc_get_system_tick); S(0x1F, svc_connect_named_port); S(0x21, svc_send_sync_request);
    S(0x24, svc_get_process_id); S(0x25, svc_get_thread_id); S(0x26, svc_break); S(0x27, svc_output_debug); S(0x28, svc_return_from_exception);
    S(0x29, svc_get_info); S(0x2A, svc_flush); S(0x2B, svc_flush); S(0x30, svc_reslimit); S(0x31, svc_reslimit_cur); S(0x32, svc_set_activity);
    S(0x33, svc_get_thread_context3); S(0x34, svc_wait_for_address); S(0x35, svc_signal_to_address); S(0x36, svc_sync_preempt);
    S(0x37, svc_reslimit_cur); S(0x45, svc_create_event);
#undef S
}

static volatile LONG g_svc_logged;
#define SVC_LOG_LIMIT 600

void rt_svc(Ctx *c, uint32_t imm) {
    uint64_t a0 = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3];
    if (imm > 0xFF) imm = 0xFF;
    InterlockedIncrement64(&g_svc_count[imm]);
    rt_ring_push("SVC", svc_name(imm), a0, a1, a2);
    SvcFn f = g_svc[imm];
    int log = g_trace || InterlockedIncrement(&g_svc_logged) <= SVC_LOG_LIMIT;
    if (g_svc_logged == SVC_LOG_LIMIT + 1 && !g_trace) rt_log("svc logging suppressed after %d lines (SMRPG_TRACE=1 logs all; counts stay in the exit summary)", SVC_LOG_LIMIT);
    if (!f) {
        rt_log("UNIMPLEMENTED svc 0x%02x %s x0=%#llx x1=%#llx x2=%#llx x3=%#llx lr=%#llx", imm, svc_name(imm), (unsigned long long)a0,
               (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)c->x[30]);
        c->x[0] = RES_NOT_IMPL;
        return;
    }
    f(c);
    if (log)
        rt_log("svc 0x%02x %s(x0=%#llx x1=%#llx x2=%#llx x3=%#llx) -> w0=%#x x1=%#llx x2=%#llx", imm, svc_name(imm), (unsigned long long)a0,
               (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, (uint32_t)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2]);
}
