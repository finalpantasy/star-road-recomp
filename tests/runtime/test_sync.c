/* Star Road Recomp AC2 tests (task-13): memory ordering and condition variables. Our own code.
 *
 * Part 1 runs hand written functions in the exact shape of lifted code (locals xN, OP_* macros from the generated ops_gen.h, so the
 * real lifter op bodies are what is tested) on several host threads: message passing with stlr/ldar and with dmb, store buffering
 * with dmb ish, exclusive load/store counters (ldxr/stxr, also the acquire/release forms), LSE ldadd, a cas spin lock.
 * Part 2 drives the kernel sync svcs (ArbitrateLock/Unlock, WaitProcessWideKeyAtomic/SignalProcessWideKey, WaitForAddress/
 * SignalToAddress) the way nn::os does: a mutex and condition variable guarding a bounded queue, plus timeout semantics.
 * Output: one line "TEST <name> <passed|failed|skipped> <detail>" per test; exit code 0 only when nothing failed. */
#include "prelude.h"
#include "rt.h"
#include "rt_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void svc_init(void);

/* ------------------------------------------------------------------ harness */
typedef struct { const char *name, *status; char detail[200]; } Result;
static int g_failed;
static void report(const char *name, const char *status, const char *fmt, ...) {
    char d[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, sizeof d, fmt, ap);
    va_end(ap);
    printf("TEST %s %s %s\n", name, status, d);
    fflush(stdout);
    if (!strcmp(status, "failed")) g_failed++;
}

static Ctx *new_ctx(void) {
    Ctx *c = calloc(1, sizeof(Ctx) + 64);
    c->thr = calloc(1, sizeof(RtThread));
    return c;
}

/* spin barrier with sense reversal */
typedef struct { volatile LONG count, sense; int n; } Barrier;
static void bar_init(Barrier *b, int n) { b->count = 0; b->sense = 0; b->n = n; }
static void bar_wait(Barrier *b) {
    LONG s = b->sense;
    if (InterlockedIncrement(&b->count) == b->n) { b->count = 0; MemoryBarrier(); b->sense = !s; }
    else while (b->sense == s) YieldProcessor();
}

typedef struct { void *(*fn)(void *); void *arg; HANDLE h; } Thr;
static DWORD WINAPI thr_main(LPVOID p) { Thr *t = p; t->fn(t->arg); return 0; }
static void thr_start(Thr *t, void *(*fn)(void *), void *arg) { t->fn = fn; t->arg = arg; t->h = CreateThread(0, 1 << 20, thr_main, t, 0, 0); }
static void thr_join(Thr *t) { WaitForSingleObject(t->h, INFINITE); CloseHandle(t->h); }

/* ------------------------------------------------------------------ lifted style functions */
/* x0 = data address, x1 = flag address, results in c->x[3] (flag seen) and c->x[4] (data seen) */
static void mp_stlr_writer(Ctx *restrict c) {
    u64 x0 = c->x[0], x1 = c->x[1], x2 = 0, x31 = 0, sp = c->x[31];
    u8 fl[4] = {0, 0, 0, 0};
    (void)x31; (void)sp; (void)fl; (void)x0; (void)x1;
    OP_movi(2, 1);
    OP_st(4, 2, 0, 0);            /* str  w2, [x0]    data = 1 */
    OP_stlr(4, 2, 1);             /* stlr w2, [x1]    flag = 1 (release) */
}
static void mp_ldar_reader(Ctx *restrict c) {
    u64 x0 = c->x[0], x1 = c->x[1], x3 = 0, x4 = 0, x31 = 0, sp = c->x[31];
    u8 fl[4] = {0, 0, 0, 0};
    (void)x31; (void)sp; (void)fl; (void)x0; (void)x1;
    OP_ldar(4, 3, 1);             /* ldar w3, [x1]    flag (acquire) */
    OP_ld(4, 0, 0, 4, 0, 0);      /* ldr  w4, [x0]    data */
    c->x[3] = x3; c->x[4] = x4;
}
static void mp_dmb_writer(Ctx *restrict c) {
    u64 x0 = c->x[0], x1 = c->x[1], x2 = 0, x31 = 0, sp = c->x[31];
    u8 fl[4] = {0, 0, 0, 0};
    (void)x31; (void)sp; (void)fl; (void)x0; (void)x1;
    OP_movi(2, 1);
    OP_st(4, 2, 0, 0);
    OP_dmb(0xA);                  /* dmb ishst */
    OP_st(4, 2, 1, 0);
}
static void mp_dmb_reader(Ctx *restrict c) {
    u64 x0 = c->x[0], x1 = c->x[1], x3 = 0, x4 = 0, x31 = 0, sp = c->x[31];
    u8 fl[4] = {0, 0, 0, 0};
    (void)x31; (void)sp; (void)fl; (void)x0; (void)x1;
    OP_ld(4, 0, 0, 3, 1, 0);
    OP_dmb(0x9);                  /* dmb ishld */
    OP_ld(4, 0, 0, 4, 0, 0);
    c->x[3] = x3; c->x[4] = x4;
}
/* store buffering: x0 = my flag, x1 = other flag; result x3 */
static void sb_full(Ctx *restrict c) {
    u64 x0 = c->x[0], x1 = c->x[1], x2 = 0, x3 = 0, x31 = 0, sp = c->x[31];
    u8 fl[4] = {0, 0, 0, 0};
    (void)x31; (void)sp; (void)fl; (void)x0; (void)x1;
    OP_movi(2, 1);
    OP_st(4, 2, 0, 0);
    OP_dmb(0xB);                  /* dmb ish */
    OP_ld(4, 0, 0, 3, 1, 0);
    c->x[3] = x3;
}
static void sb_none(Ctx *restrict c) {
    u64 x0 = c->x[0], x1 = c->x[1], x2 = 0, x3 = 0, x31 = 0, sp = c->x[31];
    u8 fl[4] = {0, 0, 0, 0};
    (void)x31; (void)sp; (void)fl; (void)x0; (void)x1;
    OP_movi(2, 1);
    OP_st(4, 2, 0, 0);
    OP_ld(4, 0, 0, 3, 1, 0);
    c->x[3] = x3;
}
/* ldxr / add / stxr loop on the u32 at x0, n times in x1 (acquire form uses the same lifted op) */
static void llsc_add(Ctx *restrict c) {
    u64 x0 = c->x[0], x1 = c->x[1], x2 = 0, x3 = 0, x4 = 0, x31 = 0, sp = c->x[31];
    u8 fl[4] = {0, 0, 0, 0};
    (void)x31; (void)sp; (void)fl; (void)x0;
    while (x1) {
    L_retry:;
        OP_ldxr(4, 2, 0);
        OP_addsub_imm(0, 0, 3, 2, 1);
        OP_stxr(4, 4, 3, 0);
        if ((u32)x4 != 0) goto L_retry;
        x1--;
    }
}
/* ldadd w2, w3, [x0] n times */
static void lse_add(Ctx *restrict c) {
    u64 x0 = c->x[0], x1 = c->x[1], x2 = 1, x3 = 0, x31 = 0, sp = c->x[31];
    u8 fl[4] = {0, 0, 0, 0};
    (void)x31; (void)sp; (void)fl; (void)x0;
    while (x1) { OP_atomic(4, 0, 2, 3, 0); x1--; }
}
/* cas spin lock at x0 protecting the u64 at x5; n iterations of lock; ldr/add/str; stlr unlock */
static void lock_incr(Ctx *restrict c) {
    u64 x0 = c->x[0], x1 = c->x[1], x5 = c->x[5], x2 = 0, x3 = 0, x4 = 0, x6 = 0, x7 = 1, x31 = 0, sp = c->x[31];
    u8 fl[4] = {0, 0, 0, 0};
    (void)x31; (void)sp; (void)fl; (void)x0; (void)x5;
    while (x1) {
        do { OP_movi(2, 0); OP_cas(4, 2, 7, 0); } while ((u32)x2 != 0);     /* cas w2, w7, [x0]: expect 0, new 1; old in w2 */
        OP_ld(8, 0, 1, 6, 5, 0);
        OP_addsub_imm(1, 0, 6, 6, 1);
        OP_st(8, 6, 5, 0);
        OP_movi(4, 0);
        OP_stlr(4, 4, 0);
        x1--;
    }
}

typedef struct { void (*fn)(Ctx *); u64 a[8]; int idx; Barrier *bar; long iters; u64 out_flag, out_data; long *hist; } Job;

/* ---- message passing */
typedef struct { volatile uint32_t data; char pad1[60]; volatile uint32_t flag; char pad2[60]; } MpSlot;
typedef struct { MpSlot *slots; long n; void (*writer)(Ctx *); void (*reader)(Ctx *); Barrier bar; long bad, seen_flag; } Mp;
static void *mp_w(void *p) {
    Mp *m = p;
    Ctx *c = new_ctx();
    bar_wait(&m->bar);
    for (long i = 0; i < m->n; i++) { c->x[0] = (u64)&m->slots[i].data; c->x[1] = (u64)&m->slots[i].flag; m->writer(c); }
    return 0;
}
static void *mp_r(void *p) {
    Mp *m = p;
    Ctx *c = new_ctx();
    bar_wait(&m->bar);
    for (long i = 0; i < m->n; i++) {
        c->x[0] = (u64)&m->slots[i].data; c->x[1] = (u64)&m->slots[i].flag;
        m->reader(c);
        if ((uint32_t)c->x[3] == 1) { m->seen_flag++; if ((uint32_t)c->x[4] != 1) m->bad++; }
    }
    return 0;
}
static void run_mp(const char *name, void (*w)(Ctx *), void (*r)(Ctx *)) {
    Mp m = {0};
    m.n = 4000000;
    m.slots = VirtualAlloc(0, sizeof(MpSlot) * m.n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    m.writer = w; m.reader = r;
    bar_init(&m.bar, 2);
    Thr tw, tr;
    thr_start(&tw, mp_w, &m);
    thr_start(&tr, mp_r, &m);
    thr_join(&tw); thr_join(&tr);
    if (m.bad) report(name, "failed", "%ld of %ld flagged reads saw stale data", m.bad, m.seen_flag);
    else if (!m.seen_flag) report(name, "skipped", "the reader never overlapped the writer");
    else report(name, "passed", "%ld flagged reads, 0 stale (4M slots)", m.seen_flag);
    VirtualFree(m.slots, 0, MEM_RELEASE);
}

/* ---- store buffering */
typedef struct { volatile uint32_t v[2][16]; } SbSlot;     /* v[0][0] = x, v[1][0] = y on separate cache lines */
typedef struct { SbSlot *slots; long n; void (*fn)(Ctx *); Barrier bar; int side; long both_zero; uint32_t *res[2]; } Sb;
typedef struct { Sb *s; int side; } SbArg;
static void *sb_thread(void *p) {
    SbArg *a = p;
    Sb *s = a->s;
    Ctx *c = new_ctx();
    for (long i = 0; i < s->n; i++) {
        bar_wait(&s->bar);
        c->x[0] = (u64)&s->slots[i].v[a->side][0];
        c->x[1] = (u64)&s->slots[i].v[1 - a->side][0];
        s->fn(c);
        s->res[a->side][i] = (uint32_t)c->x[3];
    }
    return 0;
}
static long run_sb(void (*fn)(Ctx *), long n) {
    Sb s = {0};
    s.n = n; s.fn = fn;
    s.slots = VirtualAlloc(0, sizeof(SbSlot) * n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    s.res[0] = calloc(n, 4); s.res[1] = calloc(n, 4);
    bar_init(&s.bar, 2);
    SbArg a0 = {&s, 0}, a1 = {&s, 1};
    Thr t0, t1;
    thr_start(&t0, sb_thread, &a0); thr_start(&t1, sb_thread, &a1);
    thr_join(&t0); thr_join(&t1);
    long z = 0;
    for (long i = 0; i < n; i++) if (!s.res[0][i] && !s.res[1][i]) z++;
    VirtualFree(s.slots, 0, MEM_RELEASE);
    free(s.res[0]); free(s.res[1]);
    return z;
}

/* ---- counters */
typedef struct { void (*fn)(Ctx *); volatile uint32_t *cnt; long n; Barrier *bar; volatile uint64_t *shared; volatile uint32_t *lock; } Cnt;
static void *cnt_thread(void *p) {
    Cnt *k = p;
    Ctx *c = new_ctx();
    c->x[0] = (u64)(k->lock ? (volatile void *)k->lock : (volatile void *)k->cnt);
    c->x[1] = (u64)k->n;
    c->x[5] = (u64)k->shared;
    bar_wait(k->bar);
    k->fn(c);
    return 0;
}
static void run_cnt(const char *name, void (*fn)(Ctx *), int threads, long per, int use_lock) {
    volatile uint32_t cnt = 0, lock = 0;
    volatile uint64_t shared = 0;
    Barrier bar;
    bar_init(&bar, threads);
    Cnt k = {fn, &cnt, per, &bar, &shared, use_lock ? &lock : 0};
    Thr t[16];
    for (int i = 0; i < threads; i++) thr_start(&t[i], cnt_thread, &k);
    for (int i = 0; i < threads; i++) thr_join(&t[i]);
    uint64_t got = use_lock ? shared : cnt, want = (uint64_t)threads * per;
    if (got == want) report(name, "passed", "%d threads x %ld = %llu", threads, per, (unsigned long long)got);
    else report(name, "failed", "expected %llu got %llu", (unsigned long long)want, (unsigned long long)got);
}

/* ------------------------------------------------------------------ kernel sync (rt_svc) */
#define WAITERS 0x40000000u
static uint64_t sv(Ctx *c, uint32_t imm, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    rt_svc(c, imm);
    return c->x[0];
}
static inline uint32_t ld_acq(volatile uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline int cas32(volatile uint32_t *p, uint32_t e, uint32_t d) { return __atomic_compare_exchange_n(p, &e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
/* libnx / nn::os style mutex over the kernel's arbitration svcs */
static void mtx_lock(Ctx *c, volatile uint32_t *m, uint32_t tag) {
    for (;;) {
        uint32_t cur = ld_acq(m);
        if (cur == 0) { if (cas32(m, 0, tag)) return; continue; }
        if (!(cur & WAITERS)) { if (!cas32(m, cur, cur | WAITERS)) continue; cur |= WAITERS; }
        sv(c, 0x1A, cur & ~WAITERS, (uint64_t)m, tag, 0);
        if ((ld_acq(m) & ~WAITERS) == tag) return;
    }
}
static void mtx_unlock(Ctx *c, volatile uint32_t *m, uint32_t tag) {
    if (cas32(m, tag, 0)) return;
    sv(c, 0x1B, (uint64_t)m, 0, 0, 0);
}
typedef struct {
    volatile uint32_t mtx, cv_not_empty, cv_not_full;
    int q[64], head, tail, count;
    long produced, consumed; uint64_t sum_in, sum_out;
    long total; int nprod, ncons;
    volatile LONG next_tag;
    volatile LONG done_prod;
} Q;
static void *prod(void *p) {
    Q *q = p;
    Ctx *c = new_ctx();
    uint32_t tag = (uint32_t)InterlockedIncrement(&q->next_tag);
    for (;;) {
        mtx_lock(c, &q->mtx, tag);
        if (q->produced >= q->total) { mtx_unlock(c, &q->mtx, tag); break; }
        while (q->count == 64) {
            sv(c, 0x1C, (uint64_t)&q->mtx, (uint64_t)&q->cv_not_full, tag, (uint64_t)-1);   /* WaitProcessWideKeyAtomic, unlocks and relocks */
        }
        int v = (int)(++q->produced);
        q->q[q->tail] = v; q->tail = (q->tail + 1) & 63; q->count++;
        q->sum_in += (uint64_t)v;
        sv(c, 0x1D, (uint64_t)&q->cv_not_empty, 1, 0, 0);                                      /* SignalProcessWideKey */
        mtx_unlock(c, &q->mtx, tag);
    }
    mtx_lock(c, &q->mtx, tag);
    q->done_prod++;
    sv(c, 0x1D, (uint64_t)&q->cv_not_empty, (uint64_t)-1, 0, 0);
    mtx_unlock(c, &q->mtx, tag);
    return 0;
}
static void *cons(void *p) {
    Q *q = p;
    Ctx *c = new_ctx();
    uint32_t tag = (uint32_t)InterlockedIncrement(&q->next_tag);
    for (;;) {
        mtx_lock(c, &q->mtx, tag);
        while (q->count == 0) {
            if (q->done_prod == q->nprod) { mtx_unlock(c, &q->mtx, tag); return 0; }
            sv(c, 0x1C, (uint64_t)&q->mtx, (uint64_t)&q->cv_not_empty, tag, (uint64_t)-1);
        }
        int v = q->q[q->head]; q->head = (q->head + 1) & 63; q->count--;
        q->sum_out += (uint64_t)v; q->consumed++;
        sv(c, 0x1D, (uint64_t)&q->cv_not_full, 1, 0, 0);
        mtx_unlock(c, &q->mtx, tag);
    }
}
static void run_queue(int nprod, int ncons, long total) {
    Q *q = calloc(1, sizeof *q);
    q->total = total; q->nprod = nprod; q->ncons = ncons; q->next_tag = 0x100;
    Thr t[32];
    int n = 0;
    for (int i = 0; i < ncons; i++) thr_start(&t[n++], cons, q);
    for (int i = 0; i < nprod; i++) thr_start(&t[n++], prod, q);
    int stuck = 0;
    ULONGLONG t0 = GetTickCount64();
    for (int i = 0; i < n; i++) {
        ULONGLONG el = GetTickCount64() - t0;
        DWORD left = el >= 60000 ? 0 : (DWORD)(60000 - el);
        if (WaitForSingleObject(t[i].h, left) != WAIT_OBJECT_0) { stuck = 1; break; }
    }
    char nm[64];
    snprintf(nm, sizeof nm, "condvar mutex queue %dp %dc", nprod, ncons);
    if (stuck) { report(nm, "failed", "threads still running after 60 s (lost wakeup or deadlock): produced %ld consumed %ld", q->produced, q->consumed); free(q); return; }
    uint64_t want = (uint64_t)total * (total + 1) / 2;
    if (q->produced == total && q->consumed == total && q->sum_in == want && q->sum_out == want)
        report(nm, "passed", "%ld items, every item consumed exactly once, checksum %llu", total, (unsigned long long)want);
    else report(nm, "failed", "produced %ld consumed %ld sum_in %llu sum_out %llu want %llu", q->produced, q->consumed,
                (unsigned long long)q->sum_in, (unsigned long long)q->sum_out, (unsigned long long)want);
    free(q);
}

/* a plain mutex only: many threads increment a plain counter under the arbitrated lock */
typedef struct { volatile uint32_t mtx; uint64_t count; long per; volatile LONG tag; } M;
static void *mlock_thread(void *p) {
    M *m = p;
    Ctx *c = new_ctx();
    uint32_t tag = (uint32_t)InterlockedIncrement(&m->tag);
    for (long i = 0; i < m->per; i++) { mtx_lock(c, &m->mtx, tag); m->count++; mtx_unlock(c, &m->mtx, tag); }
    return 0;
}
static void run_mutex(int threads, long per) {
    M m = {0};
    m.per = per; m.tag = 0x200;
    Thr t[16];
    for (int i = 0; i < threads; i++) thr_start(&t[i], mlock_thread, &m);
    for (int i = 0; i < threads; i++) thr_join(&t[i]);
    char nm[64];
    snprintf(nm, sizeof nm, "arbitrated mutex %d threads", threads);
    if (m.count == (uint64_t)threads * per) report(nm, "passed", "%d x %ld = %llu", threads, per, (unsigned long long)m.count);
    else report(nm, "failed", "expected %llu got %llu", (unsigned long long)threads * per, (unsigned long long)m.count);
}

/* timeout semantics: a timed out WaitProcessWideKeyAtomic returns RES_TIMEOUT and leaves the mutex unlocked; the wait takes about
 * the requested time; a signal before the timeout returns 0 with the mutex held by the waiter */
typedef struct { volatile uint32_t mtx, cv; uint64_t r; ULONGLONG ms; uint32_t mtx_after; } T;
static void *timed_waiter(void *p) {
    T *t = p;
    Ctx *c = new_ctx();
    mtx_lock(c, &t->mtx, 0x300);
    ULONGLONG t0 = GetTickCount64();
    t->r = sv(c, 0x1C, (uint64_t)&t->mtx, (uint64_t)&t->cv, 0x300, 50u * 1000000u);
    t->ms = GetTickCount64() - t0;
    t->mtx_after = ld_acq(&t->mtx);
    return 0;
}
static void test_timeout(void) {
    T t = {0};
    Thr th;
    thr_start(&th, timed_waiter, &t);
    thr_join(&th);
    if (t.r == 0xEA01u && t.ms >= 40 && t.ms < 2000 && t.mtx_after == 0)
        report("condvar timeout leaves mutex unlocked", "passed", "result 0xEA01 after %llu ms, mutex word %u", t.ms, t.mtx_after);
    else report("condvar timeout leaves mutex unlocked", "failed", "result %#llx after %llu ms, mutex word %#x", (unsigned long long)t.r, t.ms, t.mtx_after);
}
static void *sig_waiter(void *p) {
    T *t = p;
    Ctx *c = new_ctx();
    mtx_lock(c, &t->mtx, 0x301);
    t->r = sv(c, 0x1C, (uint64_t)&t->mtx, (uint64_t)&t->cv, 0x301, (uint64_t)-1);
    t->mtx_after = ld_acq(&t->mtx);
    mtx_unlock(c, &t->mtx, 0x301);
    return 0;
}
static void test_signal_reacquire(void) {
    T t = {0};
    Thr th;
    thr_start(&th, sig_waiter, &t);
    Ctx *c = new_ctx();
    Sleep(100);
    mtx_lock(c, &t.mtx, 0x302);          /* the waiter has released the mutex inside the wait */
    sv(c, 0x1D, (uint64_t)&t.cv, 1, 0, 0);
    Sleep(50);
    mtx_unlock(c, &t.mtx, 0x302);        /* the waiter can now reacquire */
    thr_join(&th);
    if (t.r == 0 && (t.mtx_after & ~WAITERS) == 0x301) report("condvar signal wakes with mutex reacquired", "passed", "result 0, mutex owner tag %#x", t.mtx_after & ~WAITERS);
    else report("condvar signal wakes with mutex reacquired", "failed", "result %#llx mutex word %#x", (unsigned long long)t.r, t.mtx_after);
}

/* WaitForAddress / SignalToAddress (type 0 less-than, signal type 0 plain) */
typedef struct { volatile uint32_t word; uint64_t r; } A;
static void *addr_waiter(void *p) {
    A *a = p;
    Ctx *c = new_ctx();
    a->r = sv(c, 0x34, (uint64_t)&a->word, 2 /* WaitIfEqual */, 0, (uint64_t)-1);
    return 0;
}
static void test_addr(void) {
    A a = {0};
    Thr th;
    thr_start(&th, addr_waiter, &a);
    Sleep(100);
    Ctx *c = new_ctx();
    a.word = 5;
    uint64_t rs = sv(c, 0x35, (uint64_t)&a.word, 0 /* Signal */, 0, 1);
    thr_join(&th);
    if (a.r == 0 && rs == 0) report("WaitForAddress wakes on SignalToAddress", "passed", "waiter result 0");
    else report("WaitForAddress wakes on SignalToAddress", "failed", "waiter %#llx signaler %#llx", (unsigned long long)a.r, (unsigned long long)rs);
    /* wait with a value that does not match returns InvalidState immediately */
    uint64_t r2 = sv(c, 0x34, (uint64_t)&a.word, 2, 99, (uint64_t)-1);
    if (r2 == 0xD401u) report("WaitForAddress mismatch returns at once", "passed", "result 0xD401");
    else report("WaitForAddress mismatch returns at once", "failed", "result %#llx", (unsigned long long)r2);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e) {
    printf("TEST crash failed exception %#lx at rip %p (image base %p), fault address %p\n", e->ExceptionRecord->ExceptionCode,
           (void *)e->ContextRecord->Rip, (void *)GetModuleHandleA(0), e->ExceptionRecord->NumberParameters > 1 ? (void *)e->ExceptionRecord->ExceptionInformation[1] : 0);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(void) {
    SetUnhandledExceptionFilter(crash_filter);
    rt_log_open("A:/AI/Recomps/smrpg-work/build/test_sync/test_sync.log");
    svc_init();
    run_mp("mp stlr/ldar", mp_stlr_writer, mp_ldar_reader);
    run_mp("mp dmb ishst/ishld", mp_dmb_writer, mp_dmb_reader);
    long with = run_sb(sb_full, 300000);
    if (with == 0) report("sb with dmb ish forbids 0/0", "passed", "0 of 300000 rounds");
    else report("sb with dmb ish forbids 0/0", "failed", "%ld of 300000 rounds ended 0/0", with);
    long without = run_sb(sb_none, 300000);
    if (without > 0) report("sb without dmb shows 0/0 (harness sensitivity)", "passed", "%ld of 300000 rounds ended 0/0, so reordering is observable", without);
    else report("sb without dmb shows 0/0 (harness sensitivity)", "skipped", "0/0 not observed in 300000 rounds on this run");
    run_cnt("ldxr/stxr counter", llsc_add, 4, 500000, 0);
    run_cnt("ldadd counter", lse_add, 4, 500000, 0);
    run_cnt("cas lock with stlr unlock", lock_incr, 4, 200000, 1);
    run_mutex(4, 100000);
    run_mutex(8, 50000);
    run_queue(1, 1, 200000);
    run_queue(4, 4, 200000);
    run_queue(2, 6, 200000);
    test_timeout();
    test_signal_reacquire();
    test_addr();
    printf("SUMMARY failed=%d\n", g_failed);
    return g_failed ? 1 : 0;
}
