/* Star Road Recomp cooperative GC safepoints (task-14, docs/RECOMPILER.md 4.5). Our own code.
 *
 * Per guest thread: susp (a pause request from svcSetThreadActivity) and state:
 *   RUNNING  executing guest code (lifted or interpreted) or a short HLE call
 *   SAFE     inside an svc or a blocking HLE call; its Ctx holds every register (flushed before the call)
 *   PARKED   stopped at a safepoint because of a pause request; Ctx holds every register
 * A pause is synchronous like the Horizon kernel's: svcSetThreadActivity(pause) returns once the target is
 * SAFE or PARKED, so the following svcGetThreadContext3 reads a quiescent Ctx. A SAFE thread that returns
 * from its svc while paused parks there. Leaving SAFE and pausing use a store-then-load handshake on both
 * sides (full barriers), so either the leaving thread sees the request or the pauser sees RUNNING and waits.
 * Lifted code polls g_gc_pending on loop back-edges and calls gc_poll only while a request exists.
 */
#include "gc.h"

volatile int32_t g_gc_pending;

enum { ST_RUNNING = 0, ST_SAFE = 1, ST_PARKED = 2 };
typedef struct GcThr {
    RtThread *volatile t;
    volatile LONG susp, state;
    volatile LONG64 parks;
} GcThr;

#define GC_MAX 1024
static GcThr g_thr[GC_MAX];
static SRWLOCK g_lk = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv_state = CONDITION_VARIABLE_INIT, g_cv_resume = CONDITION_VARIABLE_INIT;
static _Thread_local GcThr *tl_g;
static _Thread_local RtThread *tl_gt;

static volatile LONG64 g_n_suspend, g_n_resume, g_n_park_poll, g_n_park_svc, g_n_already_stopped, g_wait_us_total, g_wait_us_max, g_n_polls;

static GcThr *gc_thr(RtThread *t) {
    if (!t) return 0;
    uint32_t h = (uint32_t)((((uintptr_t)t >> 4) * 0x9E3779B97F4A7C15ull) >> 54) & (GC_MAX - 1);
    for (int i = 0; i < GC_MAX; i++, h = (h + 1) & (GC_MAX - 1)) {
        RtThread *cur = g_thr[h].t;
        if (cur == t) return &g_thr[h];
        if (!cur) {
            RtThread *won = InterlockedCompareExchangePointer((PVOID volatile *)&g_thr[h].t, t, 0);
            if (!won || won == t) return &g_thr[h];
        }
    }
    rt_log("FATAL gc: thread table full");
    rt_exit(10, "gc thread table");
}
static inline GcThr *self(Ctx *c) {
    if (!c || !c->thr) return 0;
    if (tl_gt != c->thr || !tl_g) { tl_g = gc_thr(c->thr); tl_gt = c->thr; }
    return tl_g;
}

static void park(GcThr *g, volatile LONG64 *counter) {
    AcquireSRWLockExclusive(&g_lk);
    while (g->susp) {
        if (g->state != ST_PARKED) {
            InterlockedExchange(&g->state, ST_PARKED);
            InterlockedIncrement64(&g->parks);
            InterlockedIncrement64(counter);
            WakeAllConditionVariable(&g_cv_state);
        }
        SleepConditionVariableSRW(&g_cv_resume, &g_lk, INFINITE, 0);
    }
    InterlockedExchange(&g->state, ST_RUNNING);
    ReleaseSRWLockExclusive(&g_lk);
}

void gc_poll(Ctx *c) {
    InterlockedIncrement64(&g_n_polls);
    GcThr *g = self(c);
    if (g && g->susp) park(g, &g_n_park_poll);
}

void gc_safe_enter(Ctx *c) {
    GcThr *g = self(c);
    if (!g) return;
    InterlockedExchange(&g->state, ST_SAFE);
    if (g->susp) {
        AcquireSRWLockExclusive(&g_lk);
        WakeAllConditionVariable(&g_cv_state);
        ReleaseSRWLockExclusive(&g_lk);
    }
}

void gc_safe_leave(Ctx *c) {
    GcThr *g = self(c);
    if (!g) return;
    InterlockedExchange(&g->state, ST_RUNNING);   /* full barrier, then read the request */
    if (g->susp) park(g, &g_n_park_svc);
}

static int stopped(GcThr *g, RtThread *t) { return g->state != ST_RUNNING || !t->started || t->exited; }

uint32_t gc_set_activity(Ctx *c, RtThread *t, int pause) {
    if (!t) return RES_INVALID_HANDLE;
    GcThr *g = gc_thr(t);
    if (pause) {
        if (c && t == c->thr) return RES_INVALID_STATE;          /* Horizon refuses to pause the calling thread */
        if (InterlockedCompareExchange(&g->susp, 1, 0) != 0) return RES_INVALID_STATE;
        InterlockedIncrement((volatile LONG *)&g_gc_pending);
        InterlockedIncrement64(&g_n_suspend);
        double t0 = rt_now();
        int warned = 0, waited = 0;
        AcquireSRWLockExclusive(&g_lk);
        while (!stopped(g, t)) {
            waited = 1;
            SleepConditionVariableSRW(&g_cv_state, &g_lk, 20, 0);
            double el = rt_now() - t0;
            if (el > 2.0 && !warned) {
                warned = 1;
                rt_log("gc: thread %u (%s) has not reached a safepoint 2 s after a pause request; last flushed pc=%#llx lr=%#llx",
                       t->tid, t->name, (unsigned long long)(t->ctx ? t->ctx->pc : 0), (unsigned long long)(t->ctx ? t->ctx->x[30] : 0));
            }
            if (el > 30.0) {
                ReleaseSRWLockExclusive(&g_lk);
                rt_fatal(c, "gc: a paused thread never reached a safepoint in 30 s (missing poll or a blocking HLE without gc_safe_enter)", t->tid);
            }
        }
        ReleaseSRWLockExclusive(&g_lk);
        if (!waited) InterlockedIncrement64(&g_n_already_stopped);
        LONG64 us = (LONG64)((rt_now() - t0) * 1e6);
        InterlockedAdd64(&g_wait_us_total, us);
        for (LONG64 m = g_wait_us_max; us > m; m = g_wait_us_max)
            if (InterlockedCompareExchange64(&g_wait_us_max, us, m) == m) break;
        if (g_trace) rt_log("gc: paused thread %u after %lld us", t->tid, (long long)us);
        return RES_OK;
    }
    if (InterlockedCompareExchange(&g->susp, 0, 1) != 1) return RES_INVALID_STATE;
    InterlockedDecrement((volatile LONG *)&g_gc_pending);
    InterlockedIncrement64(&g_n_resume);
    AcquireSRWLockExclusive(&g_lk);
    WakeAllConditionVariable(&g_cv_resume);
    ReleaseSRWLockExclusive(&g_lk);
    return RES_OK;
}

int gc_thread_stopped(RtThread *t) {
    GcThr *g = gc_thr(t);
    return g && g->susp && stopped(g, t);
}

void gc_summary(void) {
    rt_log("gc: pauses %lld, resumes %lld, parked at poll %lld, parked at svc exit %lld, already stopped %lld, slow polls %lld, wait total %.3f ms max %.3f ms",
           (long long)g_n_suspend, (long long)g_n_resume, (long long)g_n_park_poll, (long long)g_n_park_svc, (long long)g_n_already_stopped,
           (long long)g_n_polls, (double)g_wait_us_total / 1000.0, (double)g_wait_us_max / 1000.0);
}
