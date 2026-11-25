/* Star Road Recomp guest thread control (task-13), the implementation of hle_thread.h. Our own code. */
#include "hle_thread.h"

#define MAXT 1024
static volatile LONG g_susp[MAXT];            /* suspend nesting per registry slot */
static CRITICAL_SECTION g_world_cs;
static INIT_ONCE g_world_once = INIT_ONCE_STATIC_INIT;
static RtThread *g_stopped[MAXT];
static int g_nstopped;
static BOOL CALLBACK world_once_cb(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&g_world_cs); return TRUE; }

static int slot_of(RtThread *const *tab, int n, RtThread *t) { for (int i = 0; i < n && i < MAXT; i++) if (tab[i] == t) return i; return -1; }
static int live(RtThread *t) { return t && t->host && t->started && !t->exited; }

static void fill(HleThreadInfo *o, RtThread *t, int slot) {
    memset(o, 0, sizeof *o);
    o->tid = t->tid; o->handle = t->handle; o->prio = t->prio; o->core = t->core;
    o->suspended = slot >= 0 ? (int)g_susp[slot] : 0;
    o->stack_base = t->stack_base; o->stack_top = t->stack_top; o->tlr = t->tlr;
    o->ctx = t->ctx; o->host = t->host; o->running = live(t);
    snprintf(o->name, sizeof o->name, "%s", t->name);
}

int hle_thread_list(HleThreadInfo *out, int max) {
    RtThread *loc[MAXT];
    int n = svc_thread_registry(loc, MAXT);
    int w = 0;
    for (int i = 0; i < n && w < max; i++) if (live(loc[i])) fill(&out[w++], loc[i], i);
    return w;
}
int hle_thread_find(uint32_t tid, HleThreadInfo *out) {
    RtThread *loc[MAXT];
    int n = svc_thread_registry(loc, MAXT);
    for (int i = 0; i < n; i++) if (loc[i]->tid == tid) { fill(out, loc[i], i); return 0; }
    return -1;
}

static int susp_one(RtThread **tab, int n, RtThread *t) {
    if (!live(t)) return -1;
    if (t == tl_ctx->thr) return -2;
    int s = slot_of(tab, n, t);
    if (SuspendThread(t->host) == (DWORD)-1) return -1;
    if (s >= 0) InterlockedIncrement(&g_susp[s]);
    return 0;
}
static int resume_one(RtThread **tab, int n, RtThread *t) {
    if (!t || !t->host) return -1;
    int s = slot_of(tab, n, t);
    if (s >= 0 && g_susp[s] <= 0) return -1;
    if (ResumeThread(t->host) == (DWORD)-1) return -1;
    if (s >= 0) InterlockedDecrement(&g_susp[s]);
    return 0;
}
static RtThread *by_tid(RtThread **tab, int n, uint32_t tid) { for (int i = 0; i < n; i++) if (tab[i]->tid == tid) return tab[i]; return 0; }

int hle_thread_suspend(uint32_t tid) {
    RtThread *tab[MAXT];
    int n = svc_thread_registry(tab, MAXT);
    RtThread *t = by_tid(tab, n, tid);
    return t ? susp_one(tab, n, t) : -1;
}
int hle_thread_resume(uint32_t tid) {
    RtThread *tab[MAXT];
    int n = svc_thread_registry(tab, MAXT);
    RtThread *t = by_tid(tab, n, tid);
    return t ? resume_one(tab, n, t) : -1;
}
int hle_thread_host_context(uint32_t tid, CONTEXT *out) {
    RtThread *tab[MAXT];
    int n = svc_thread_registry(tab, MAXT);
    RtThread *t = by_tid(tab, n, tid);
    if (!t || !t->host) return -1;
    memset(out, 0, sizeof *out);
    out->ContextFlags = CONTEXT_FULL;
    return GetThreadContext(t->host, out) ? 0 : -1;
}
int hle_thread_set_activity(RtThread *t, int paused) {
    RtThread *tab[MAXT];
    int n = svc_thread_registry(tab, MAXT);
    if (!t->host) return 0;                       /* not started yet: nothing to pause */
    int r = paused ? susp_one(tab, n, t) : resume_one(tab, n, t);
    return r == -1 ? (int)MAKE_RES(1, 111) : 0;   /* invalid state */
}

int hle_stop_world(void) {
    InitOnceExecuteOnce(&g_world_once, world_once_cb, 0, 0);
    EnterCriticalSection(&g_world_cs);
    hle_ipc_lock();
    RtThread *tab[MAXT];
    int n = svc_thread_registry(tab, MAXT);
    g_nstopped = 0;
    for (int i = 0; i < n; i++)
        if (live(tab[i]) && tab[i] != tl_ctx->thr && susp_one(tab, n, tab[i]) == 0) g_stopped[g_nstopped++] = tab[i];
    return g_nstopped;
}
void hle_resume_world(void) {
    RtThread *tab[MAXT];
    int n = svc_thread_registry(tab, MAXT);
    for (int i = 0; i < g_nstopped; i++) resume_one(tab, n, g_stopped[i]);
    g_nstopped = 0;
    hle_ipc_unlock();
    LeaveCriticalSection(&g_world_cs);
}
