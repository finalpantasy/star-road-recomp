/* task-14 AC2 GC stress test (our own code). A small conservative mark-and-sweep collector, driven exactly like Boehm
 * on Horizon: it pauses each mutator with svcSetThreadActivity, reads its registers with svcGetThreadContext3, scans
 * registers and stack conservatively, sweeps (poisoning freed nodes), then resumes. Mutators build linked lists whose
 * heads live only in registers and on the stack, in loops with no svc (only back-edge safepoints can stop them).
 * Live-object checker: a mutator that ever reads a poisoned node reports a violation.
 * Last lines: "GC DONE rounds=<n>", "GC DONE freed=<n>", "GC DONE violations=<n>", "GC DONE verified=<n>". */
#include "guest.h"

#define NMUT 3
#define NODES 4096                    /* per mutator */
#define MAGIC 0x600DF00Dul
#define POISON 0xDEADDEADul
#define STACK (64 * 1024)

typedef struct Node { u64 magic; struct Node *next; u64 val, owner; } Node;
static Node g_heap[NMUT][NODES];
static unsigned char g_alloc[NMUT][NODES], g_mark[NMUT][NODES];
static Node *volatile g_free[NMUT];
static volatile long g_violations, g_verified, g_done[NMUT], g_starved[NMUT];
static unsigned char g_stacks[NMUT][STACK] __attribute__((aligned(16)));
static u64 g_ctx[0x320 / 8];
static volatile int g_skip_regs;               /* negative control: scan stacks only, so register-only roots get freed */

static Node *alloc_node(int t) {
    Node *n;
    while (!(n = g_free[t])) { g_starved[t]++; __asm__ volatile("" ::: "memory"); }   /* back-edge: waits for a collection */
    g_free[t] = n->next;
    g_alloc[t][n - g_heap[t]] = 1;
    n->magic = MAGIC; n->next = 0; n->owner = (u64)t;
    return n;
}

static long verify(Node *h, u64 expect_len) {
    long len = 0;
    for (Node *p = h; p; p = p->next) {
        /* a poisoned node, or a list longer than built (a freed node was reused and relinked): premature free */
        if (p->magic != MAGIC || len > (long)expect_len) { __atomic_fetch_add(&g_violations, 1, __ATOMIC_RELAXED); return -1; }
        len++;
    }
    __atomic_fetch_add(&g_verified, 1, __ATOMIC_RELAXED);
    return len == (long)expect_len ? len : -2;
}

/* lists held only in locals: the compiler keeps the heads in callee-saved registers across the loop */
__attribute__((noinline)) static void mutator(u64 t) {
    Node *a = 0, *b = 0;
    u64 la = 0, lb = 0;
    volatile Node *on_stack[8] = {0};
    for (long i = 0; i < 60000 && !(g_skip_regs && g_violations); i++) {
        Node *n = alloc_node((int)t);
        n->val = (u64)i;
        if (i & 1) { n->next = a; a = n; la++; } else { n->next = b; b = n; lb++; }
        if ((i & 255) == 0) on_stack[(i >> 8) & 7] = n;          /* some roots only on the stack */
        if (la == 300) { if (verify(a, la) < 0 && g_violations == 0) g_violations++; a = 0; la = 0; }   /* drop: garbage */
        if (lb == 500) { if (verify(b, lb) < 0 && g_violations == 0) g_violations++; b = 0; lb = 0; }
    }
    for (int k = 0; k < 8; k++)
        if (on_stack[k] && on_stack[k]->magic != MAGIC) g_violations++;
    if (!(g_skip_regs && g_violations)) { verify(a, la); verify(b, lb); }
    g_done[t] = 1;
}
static void mutator_entry(u64 t) { mutator(t); for (;;) svc6(0x0B, 1000000000ul, 0, 0, 0, 0, 0, 0); }

static int heap_index(u64 w, int *t, int *i) {
    u64 lo = (u64)&g_heap[0][0], hi = (u64)&g_heap[NMUT][0];
    if (w < lo || w >= hi) return 0;
    u64 o = (w - lo) / sizeof(Node);
    *t = (int)(o / NODES); *i = (int)(o % NODES);
    return g_alloc[*t][*i];
}
static void mark_word(u64 w) {
    int t, i;
    while (heap_index(w, &t, &i) && !g_mark[t][i]) {             /* follow next pointers (the only pointer field) */
        g_mark[t][i] = 1;
        w = (u64)g_heap[t][i].next;
    }
}

static void collect(u64 *handles, long *freed) {
    for (int t = 0; t < NMUT; t++) if (svc6(0x32, handles[t], 1, 0, 0, 0, 0, 0)) out("FAIL gc: pause refused");
    for (int t = 0; t < NMUT; t++) for (int i = 0; i < NODES; i++) g_mark[t][i] = 0;
    for (int t = 0; t < NMUT; t++) {
        if (svc6(0x33, (u64)g_ctx, handles[t], 0, 0, 0, 0, 0)) { out("FAIL gc: GetThreadContext3 refused"); continue; }
        if (!g_skip_regs) {
            for (int r = 0; r < 31; r++) mark_word(g_ctx[r]);
            for (int r = 0; r < 64; r++) mark_word(g_ctx[0x110 / 8 + r]);   /* v0..v31 halves */
        }
        u64 sp = g_ctx[31], top = (u64)&g_stacks[t][STACK];
        if (sp < (u64)&g_stacks[t][0] || sp > top) { out_num("FAIL gc: sp outside the stack, thread ", t); continue; }
        for (u64 p = sp & ~7ul; p < top; p += 8) mark_word(*(u64 *)p);
    }
    for (int t = 0; t < NMUT; t++)
        for (int i = 0; i < NODES; i++)
            if (g_alloc[t][i] && !g_mark[t][i]) {
                Node *n = &g_heap[t][i];
                n->magic = POISON; n->val = POISON;
                g_alloc[t][i] = 0;
                n->next = g_free[t]; g_free[t] = n;
                (*freed)++;
            }
    for (int t = 0; t < NMUT; t++) if (svc6(0x32, handles[t], 0, 0, 0, 0, 0, 0)) out("FAIL gc: resume refused");
}

static void gc_run(void);
void gc_main(void) { out("GC START"); gc_run(); }
/* must report violations: proves the heads live only in registers at the safepoints and that the checker sees a
 * premature free */
void gc_main_control(void) { out("GC START control (registers not scanned)"); g_skip_regs = 1; gc_run(); }
static void gc_run(void) {
    for (int t = 0; t < NMUT; t++) {
        g_free[t] = 0;
        for (int i = NODES - 1; i >= 0; i--) { g_heap[t][i].magic = POISON; g_heap[t][i].next = g_free[t]; g_free[t] = &g_heap[t][i]; }
    }
    u64 h[NMUT];
    for (int t = 0; t < NMUT; t++) {
        u64 r1 = 0;
        if (svc6(0x08, 0, (u64)&mutator_entry, (u64)t, (u64)&g_stacks[t][STACK], 44, 0, &r1)) { out("FAIL gc: CreateThread"); return; }
        h[t] = r1;
    }
    for (int t = 0; t < NMUT; t++) svc6(0x09, h[t], 0, 0, 0, 0, 0, 0);
    long rounds = 0, freed = 0;
    for (;;) {
        int all = 1;
        for (int t = 0; t < NMUT; t++) all &= g_done[t] != 0;
        if (all) break;
        collect(h, &freed);
        rounds++;
        if (rounds > 200000) { out("FAIL gc: mutators never finished"); break; }
        svc6(0x0B, 200000, 0, 0, 0, 0, 0, 0);                  /* 0.2 ms */
    }
    collect(h, &freed);
    rounds++;
    out_num("GC DONE rounds=", rounds);
    out_num("GC DONE freed=", freed);
    out_num("GC DONE verified=", g_verified);
    out_num("GC DONE starved=", g_starved[0] + g_starved[1] + g_starved[2]);
    out_num("GC DONE violations=", g_violations);
}
