#include "rt_internal.h"
#include "swap.h"
#include <stdatomic.h>

typedef uint64_t (*IntLeaf)(uint64_t, uint64_t, uint64_t);
/* Store leaves take a fourth argument added to every store address (0 in normal use; see compare()). */
typedef uint64_t (*StoreLeaf)(uint64_t, uint64_t, uint64_t, uint64_t);
/* Functions that call ported leaves (task-44) edit the whole guest register file in place (Ctx.x). */
typedef void (*RegsLeaf)(uint64_t *);
/* Float and SIMD leaves (task-50): x0..x2, the low 64 bits of the vector register, the store redirect. A load or a
 * constant returns the new low 64 bits (every other lane of the register becomes zero); a store returns 0. */
typedef uint64_t (*VecLeaf)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#define MAX_FOOTPRINT 32
typedef struct {
    uint64_t address;
    GuestFn entry, lifted;
    IntLeaf rust;
    StoreLeaf rust_store;           /* set instead of rust for a leaf that writes guest memory */
    RegsLeaf rust_regs;             /* set instead of rust for a function that calls ported leaves */
    VecLeaf rust_vec;               /* set instead of rust for a float or SIMD leaf (VLEAF) */
    unsigned base_reg, offset, bytes; /* store footprint (or generated load): bytes at x[base_reg] + offset */
    unsigned load_kind;             /* generated loads: 1 = getter (GLEAF), 2 = null-safe (NLEAF), 3 = through a field (DLEAF, VLEAF 4) */
    unsigned offset2;               /* second-level load offset of a load_kind 3 row */
    unsigned vkind, vreg;           /* VLEAF: 1 load, 2 store, 3 constant, 4 load through a field; the register */
    unsigned clear_fpsr;            /* ELEAF: compare with the cumulative FPSR bits cleared on both sides (task-54) */
    unsigned replay;                /* ELEAF: a port whose callees have side effects, compared by replay (task-52) */
    unsigned probe;                 /* PLEAF: a traced row, counts live calls and runs the lifted body (task-52) */
    unsigned out_reg, out_off, out_bytes, out_in; /* OLEAF: a traced row whose callee writes out_bytes bytes at x[out_reg] + out_off; out_in: the bytes are also an input (task-57) */
    int (*pre)(uint64_t *);         /* QLEAF: pre-state hook, run before the lifted body; nonzero = defer the comparison (task-55) */
    atomic_ullong deferred;         /* QLEAF: calls whose comparison was deferred (lazy initialization still pending) */
    atomic_ullong calls, differences;
    atomic_ullong raced;            /* load rows: first pair differed, the identical retry agreed (another thread wrote the field) */
} Leaf;

#define LEAF(chunk, addr, rust) \
    void f_##addr(Ctx *); \
    extern void f_##addr##_lifted(Ctx *); \
    extern uint64_t rust(uint64_t, uint64_t, uint64_t);
#define SLEAF(chunk, addr, rust, base, off, len) \
    void f_##addr(Ctx *); \
    extern void f_##addr##_lifted(Ctx *); \
    extern uint64_t rust(uint64_t, uint64_t, uint64_t, uint64_t);
#define CLEAF(chunk, addr, rust) \
    void f_##addr(Ctx *); \
    extern void f_##addr##_lifted(Ctx *); \
    extern void rust(uint64_t *);
/* Generated rows (tools/decompgen): GLEAF and NLEAF are loads at x[base] + off, len bytes wide, called like LEAF */
#define GLEAF(chunk, addr, rust, base, off, len) LEAF(chunk, addr, rust)
#define NLEAF(chunk, addr, rust, base, off, len) LEAF(chunk, addr, rust)
#define DLEAF(chunk, addr, rust, base, off, len, off2) LEAF(chunk, addr, rust)
#define VLEAF(chunk, addr, rust, vkind, vreg, base, off, len, off2) \
    void f_##addr(Ctx *); \
    extern void f_##addr##_lifted(Ctx *); \
    extern uint64_t rust(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#define PLEAF(chunk, addr)     void f_##addr(Ctx *);     extern void f_##addr##_lifted(Ctx *);
#define OLEAF(chunk, addr, reg, off, bytes, in) PLEAF(chunk, addr)
#define ELEAF(chunk, addr, rust) CLEAF(chunk, addr, rust)
#define QLEAF(chunk, addr, rust, hook) CLEAF(chunk, addr, rust) extern int hook(uint64_t *);
#include "manifest.def"
#undef QLEAF
#undef ELEAF
#undef OLEAF
#undef PLEAF
#undef LEAF
#undef SLEAF
#undef CLEAF
#undef GLEAF
#undef NLEAF
#undef DLEAF
#undef VLEAF

enum {
#define LEAF(chunk, addr, rust) index_##addr,
#define SLEAF(chunk, addr, rust, base, off, len) index_##addr,
#define CLEAF(chunk, addr, rust) index_##addr,
#define GLEAF(chunk, addr, rust, base, off, len) index_##addr,
#define NLEAF(chunk, addr, rust, base, off, len) index_##addr,
#define DLEAF(chunk, addr, rust, base, off, len, off2) index_##addr,
#define VLEAF(chunk, addr, rust, vkind, vreg, base, off, len, off2) index_##addr,
#define PLEAF(chunk, addr) index_##addr,
#define OLEAF(chunk, addr, reg, off, bytes, in) index_##addr,
#define ELEAF(chunk, addr, rust) index_##addr,
#define QLEAF(chunk, addr, rust, hook) index_##addr,
#include "manifest.def"
#undef QLEAF
#undef ELEAF
#undef OLEAF
#undef PLEAF
#undef LEAF
#undef SLEAF
#undef CLEAF
#undef GLEAF
#undef NLEAF
#undef DLEAF
#undef VLEAF
    leaf_count
};

static Leaf leaves[leaf_count] = {
#define HEAD(addr) .address = 0x##addr##ULL, .entry = f_##addr, .lifted = f_##addr##_lifted
#define LEAF(chunk, addr, sym) {HEAD(addr), .rust = sym},
#define SLEAF(chunk, addr, sym, base, off, len) \
    {HEAD(addr), .rust_store = sym, .base_reg = base, .offset = off, .bytes = len},
#define CLEAF(chunk, addr, sym) {HEAD(addr), .rust_regs = sym},
#define GLEAF(chunk, addr, sym, base, off, len) \
    {HEAD(addr), .rust = sym, .base_reg = base, .offset = off, .bytes = len, .load_kind = 1},
#define NLEAF(chunk, addr, sym, base, off, len) \
    {HEAD(addr), .rust = sym, .base_reg = base, .offset = off, .bytes = len, .load_kind = 2},
#define DLEAF(chunk, addr, sym, base, off, len, off2) \
    {HEAD(addr), .rust = sym, .base_reg = base, .offset = off, .bytes = len, .load_kind = 3, .offset2 = off2},
#define VLEAF(chunk, addr, sym, vkind_, vreg_, base, off, len, off2) \
    {HEAD(addr), .rust_vec = sym, .base_reg = base, .offset = off, .bytes = len, \
     .load_kind = (vkind_) == 1 ? 1 : (vkind_) == 4 ? 3 : 0, .offset2 = off2, .vkind = vkind_, .vreg = vreg_},
#define PLEAF(chunk, addr) {HEAD(addr), .probe = 1},
#define OLEAF(chunk, addr, reg_, off_, bytes_, in_) {HEAD(addr), .probe = 1, .out_reg = reg_, .out_off = off_, .out_bytes = bytes_, .out_in = in_},
#define ELEAF(chunk, addr, sym) {HEAD(addr), .rust_regs = sym, .replay = 1, .clear_fpsr = 1},
#define QLEAF(chunk, addr, sym, hook) {HEAD(addr), .rust_regs = sym, .replay = 1, .clear_fpsr = 1, .pre = hook},
#include "manifest.def"
#undef QLEAF
#undef ELEAF
#undef OLEAF
#undef PLEAF
#undef LEAF
#undef SLEAF
#undef CLEAF
#undef GLEAF
#undef NLEAF
#undef DLEAF
#undef VLEAF
#undef HEAD
};

static enum { OFF, ON, AB } mode;

/* A float or SIMD leaf: x0 is untouched, a load or constant writes {bits, 0} to the vector register (a float or
 * double load, fmov or movi zeroes every other lane), a store writes guest memory (shifted by delta). */
static void run_vec(const Leaf *leaf, Ctx *c, uint64_t delta) {
    uint64_t r = leaf->rust_vec(c->x[0], c->x[1], c->x[2], (uint64_t)c->v[leaf->vreg][0], delta);
    if (leaf->vkind != 2) {
        v128 out = {(long long)r, 0};
        c->v[leaf->vreg] = out;
    }
}

static int is_store(const Leaf *leaf) { return leaf->rust_store || (leaf->rust_vec && leaf->vkind == 2); }

static void run_rust(const Leaf *leaf, Ctx *c) {
    if (leaf->rust_vec) {
        run_vec(leaf, c, 0);
        return;
    }
    if (leaf->rust_regs) {
        leaf->rust_regs(c->x);
        return;
    }
    c->x[0] = leaf->rust_store ? leaf->rust_store(c->x[0], c->x[1], c->x[2], 0)
                               : leaf->rust(c->x[0], c->x[1], c->x[2]);
}

/* Store leaves, ab mode. Rust never touches live memory: its stores are redirected (delta argument) into a
 * private shadow buffer that starts as a copy of the footprint, with the same low 4 address bits so atomic
 * store alignment matches. Lifted C then runs on live memory exactly as without the swap. Afterwards the
 * registers and the footprint bytes (live vs shadow) must match. Residual race: a different thread writing
 * the footprint between the snapshot and the final compare gives a false difference (the leaves are setters
 * on one object at a time; none seen). It cannot corrupt the game, since live memory only sees lifted code. */
static void compare_store(Leaf *leaf, Ctx *c) {
    _Alignas(16) unsigned char shadow_storage[MAX_FOOTPRINT + 16];
    const uint64_t footprint = c->x[leaf->base_reg] + leaf->offset;
    unsigned char *shadow = shadow_storage + (footprint & 15);
    memcpy(shadow, (const void *)(uintptr_t)footprint, leaf->bytes);
    Ctx trial;
    memcpy(&trial, c, sizeof trial);
    if (leaf->rust_vec) run_vec(leaf, &trial, (uint64_t)(uintptr_t)shadow - footprint);
    else trial.x[0] = leaf->rust_store(trial.x[0], trial.x[1], trial.x[2], (uint64_t)(uintptr_t)shadow - footprint);
    leaf->lifted(c);
    int regs_differ = memcmp(&trial, c, sizeof trial) != 0;
    int memory_differs = memcmp(shadow, (const void *)(uintptr_t)footprint, leaf->bytes) != 0;
    if (regs_differ || memory_differs) {
        atomic_fetch_add(&leaf->differences, 1);
        rt_log("decomp mismatch addr=%#llx regs=%d memory=%d", (unsigned long long)leaf->address, regs_differ,
               memory_differs);
    }
    atomic_fetch_add(&leaf->calls, 1);
}

/* ---- Replay rows (ELEAF, task-52) ----
 * A readable port whose callees have side effects cannot run next to the lifted body in `ab` mode: every callee
 * would run twice on live memory. Such a row is compared by replay instead. The lifted body runs once on live
 * memory, exactly as without the swap, and every callee it calls through a traced row (PLEAF) is recorded: the
 * callee's address, the whole register file at the call and the whole register file after it. Then the Rust port
 * runs on a copy of the context from before the call. Its guest calls (bridge, smrpg_guest_bl) do not execute: each
 * one is matched with the next recorded call, which must have the same callee and the same registers x0 to x30
 * and v0 to v31 at the call, and then receives the recorded register file after it. At the end the whole register
 * file of the port must equal the lifted one and every recorded call must have been used. What this does and does
 * not cover is in docs/DECOMP.md. A traced row that is reached while another call is being recorded (nested) is not
 * recorded; a callee reached through an untraced function is not seen, so the row's callees must all be traced. */
/* The recorded calls live in a per-thread heap buffer that grows (task-57; the first limit, 12 calls on the stack, was
 * too small for the damage driver, which makes about 60 traced calls). It grows by doubling up to REC_LIMIT and a
 * body that makes more traced calls than that stops the run (exit 10): a silently skipped comparison could hide a
 * difference. An out footprint (OLEAF) is kept next to the call. */
#define REC_START 64
#define REC_LIMIT 4096
#define OUT_MAX 24
typedef struct { uint64_t target; Ctx pre, post; unsigned char out_pre[OUT_MAX], out_post[OUT_MAX]; } RecCall;
typedef struct { RecCall *calls; unsigned count, next, cap; int bad; int dumped; int hide_callee_fpsr; uint32_t raised; } Recording;
#define FPSR_CUM 0x0800009fu   /* IOC DZC OFC UFC IXC IDC QC: the cumulative FPSR bits */
static __thread Recording *tl_rec, *tl_replay;
static __thread unsigned tl_depth;
static __thread uint64_t tl_stub;   /* selftest: nonzero = traced callees do not run, they clobber registers pseudo-randomly */

/* ---- Pre-state and store journal for replay rows (task-55) ----
 * The port of a replay row runs after the lifted body, so it sees memory the lifted body (or a callee it ran) has
 * already changed. Lazy initialization is the case that matters: a method tests a flag byte, runs the initializer
 * and sets the flag, and a port that runs second would find the flag set and skip a call the lifted body made.
 * A row with a pre-state hook (QLEAF) solves it in three parts:
 *  - before the lifted body runs, the hook calls smrpg_guest_snapshot for every cell the port will read or write
 *    (address, width, and whether the port itself stores to it); the hook returns nonzero when the state cannot be
 *    captured (a first call, with the metadata slots still unresolved), and that call is run lifted only and counted
 *    as deferred, never compared;
 *  - in the trial the port reads those cells with smrpg_guest_pre_read, which returns the snapshot value;
 *  - the port's stores go through smrpg_guest_store: outside a trial it stores, in a trial it only journals, and
 *    after the trial every journaled store must equal what the lifted body left in memory, and every watched cell
 *    the lifted body changed must have a journaled store of that value. */
#define PRE_MAX 96   /* task-60: the damage_calc driver names the cells of its five nested ports too */
typedef struct { uint64_t addr; unsigned size, watch; uint64_t value; } PreCell;
typedef struct { uint64_t addr; unsigned size; uint64_t value; } StoreRec;
static __thread PreCell tl_pre[PRE_MAX];
static __thread unsigned tl_pre_n;
static __thread StoreRec tl_st[PRE_MAX];
static __thread unsigned tl_st_n;
static __thread int tl_st_overflow;

static uint64_t raw_load(uint64_t addr, unsigned size) {
    switch (size) {
    case 1: return __atomic_load_n((uint8_t *)(uintptr_t)addr, __ATOMIC_RELAXED);
    case 4: return __atomic_load_n((uint32_t *)(uintptr_t)addr, __ATOMIC_RELAXED);
    default: return __atomic_load_n((uint64_t *)(uintptr_t)addr, __ATOMIC_RELAXED);
    }
}

static void raw_store(uint64_t addr, unsigned size, uint64_t value) {
    switch (size) {
    case 1: __atomic_store_n((uint8_t *)(uintptr_t)addr, (uint8_t)value, __ATOMIC_RELAXED); break;
    case 4: __atomic_store_n((uint32_t *)(uintptr_t)addr, (uint32_t)value, __ATOMIC_RELAXED); break;
    default: __atomic_store_n((uint64_t *)(uintptr_t)addr, value, __ATOMIC_RELAXED); break;
    }
}

void smrpg_guest_snapshot(uint64_t addr, unsigned size, unsigned watch) {
    if (tl_pre_n >= PRE_MAX) rt_exit(10, "decomp pre-state: too many cells");
    tl_pre[tl_pre_n++] = (PreCell){addr, size, watch, raw_load(addr, size)};
}

uint64_t smrpg_guest_pre_read(uint64_t addr, unsigned size) {
    if (tl_replay) {
        /* task-60: a port reads what it stored itself first (a stack cell a later nested port reads again), then the snapshot */
        for (unsigned i = tl_st_n; i-- > 0;)
            if (tl_st[i].addr == addr && tl_st[i].size == size) return tl_st[i].value;
        for (unsigned i = 0; i < tl_pre_n; ++i)
            if (tl_pre[i].addr == addr && tl_pre[i].size == size) return tl_pre[i].value;
    }
    return raw_load(addr, size);
}

void smrpg_guest_store(uint64_t addr, unsigned size, uint64_t value) {
    if (!tl_replay) {
        raw_store(addr, size, value);
        return;
    }
    if (tl_st_n >= PRE_MAX) tl_st_overflow = 1;
    else tl_st[tl_st_n++] = (StoreRec){addr, size, value};
}

/* task-60: a nested port's stack frame is dead once it returned, and the lifted body's later callees overwrite it: forget its stores. */
void smrpg_guest_forget_stores(uint64_t lo, uint64_t hi) {
    if (!tl_replay) return;
    unsigned kept = 0;
    for (unsigned i = 0; i < tl_st_n; ++i)
        if (tl_st[i].addr < lo || tl_st[i].addr >= hi) tl_st[kept++] = tl_st[i];
    tl_st_n = kept;
}

/* After the trial: 0 when the journal and the lifted body's memory agree, otherwise the first problem is logged. */
static int check_store_journal(Leaf *leaf) {
    int bad = tl_st_overflow;
    if (bad) rt_log("decomp store journal addr=%#llx: more than %d stores", (unsigned long long)leaf->address, PRE_MAX);
    for (unsigned i = 0; i < tl_st_n; ++i) {
        /* a cell stored twice (a default, then the real value) must match memory only at its last store (task-56) */
        int superseded = 0;
        for (unsigned k = i + 1; k < tl_st_n; ++k)
            if (tl_st[k].addr == tl_st[i].addr && tl_st[k].size == tl_st[i].size) superseded = 1;
        if (superseded) continue;
        uint64_t now = raw_load(tl_st[i].addr, tl_st[i].size);
        if (now != tl_st[i].value) {
            bad = 1;
            rt_log("decomp store journal addr=%#llx: port stored %#llx to %#llx, memory has %#llx",
                   (unsigned long long)leaf->address, (unsigned long long)tl_st[i].value, (unsigned long long)tl_st[i].addr,
                   (unsigned long long)now);
        }
    }
    for (unsigned i = 0; i < tl_pre_n; ++i) {
        if (!tl_pre[i].watch) continue;
        uint64_t now = raw_load(tl_pre[i].addr, tl_pre[i].size);
        if (now == tl_pre[i].value) continue;
        int covered = 0;
        for (unsigned k = 0; k < tl_st_n; ++k)
            if (tl_st[k].addr == tl_pre[i].addr && tl_st[k].size == tl_pre[i].size && tl_st[k].value == now) covered = 1;
        if (!covered) {
            bad = 1;
            rt_log("decomp store journal addr=%#llx: the lifted body changed %#llx to %#llx and the port did not store it",
                   (unsigned long long)leaf->address, (unsigned long long)tl_pre[i].addr, (unsigned long long)now);
        }
    }
    return bad;
}

/* A stand-in callee for the selftest (the fake object graph cannot support the real one): an arbitrary result and
 * clobbered caller-saved registers, like a real callee leaves. x19 to x29, x30, sp and the exception state stay. */
static void stub_callee(Ctx *c) {
    uint64_t r = tl_stub;
    for (int i = 0; i < 5; ++i) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; }
    uint64_t w = r;
    for (int i = 0; i < 19; ++i) {
        w ^= w << 13; w ^= w >> 7; w ^= w << 17;
        if (i < 18 && i != 0 && i != 1 && i != 8) c->x[i] = w;       /* x2..x17 except the result registers set below */
        if (i == 0) c->x[0] = (w & 4) ? (w >> 8) : (w & 1);          /* bool-like or arbitrary result */
        if (i == 1) c->x[1] = w;
        if (i == 8) c->x[8] = w;
    }
    for (int i = 0; i < 8; ++i) { w ^= w << 13; w ^= w >> 7; w ^= w << 17; c->v[i][0] = (long long)w; c->v[i][1] = 0; }
    tl_stub = w | 1;
}

static __thread uint64_t tl_stub_instance;   /* selftest: what the stand-in GlobalWork.get_inst returns (a fake object, never a random word) */
static __thread uint64_t tl_stub_new;        /* selftest (task-58): what the stand-in Object.new (0x71000c44d0) returns: a fake List<int> */
static __thread uint64_t tl_stub_current;    /* selftest (task-59): what the stand-in List<T>.Enumerator.get_Current (0x7100183d20) returns: a fake element */

static void run_traced(Leaf *leaf, Ctx *c) {
    if (tl_stub) {
        stub_callee(c);
        if (leaf->address == 0x7101886a00ULL && tl_stub_instance) c->x[0] = tl_stub_instance;
        if (leaf->address == 0x71000c44d0ULL && tl_stub_new) c->x[0] = tl_stub_new;
        if (leaf->address == 0x7100183d20ULL && tl_stub_current) c->x[0] = tl_stub_current;
    } else {
        leaf->lifted(c);
    }
}

static void compare_replay(Leaf *leaf, Ctx *c) {
    if (tl_rec || tl_replay) {   /* reached from inside another recorded call: no nested comparison */
        leaf->lifted(c);
        return;
    }
    /* FPSR cumulative flags are sticky and the game sets IXC early, so a port that forgets a flag would look equal.
     * Both sides therefore start with the cumulative bits cleared and the flags each raised are compared; the
     * original bits are ORed back into the live context afterwards (game behavior unchanged). Task-54. */
    tl_pre_n = tl_st_n = 0;
    tl_st_overflow = 0;
    if (leaf->pre && leaf->pre(c->x)) {   /* the state a port needs cannot be captured: run lifted only, compare nothing */
        unsigned long long n = atomic_fetch_add(&leaf->deferred, 1);
        if (n < 3) rt_log("decomp replay deferred addr=%#llx (lazy initialization pending)", (unsigned long long)leaf->address);
        leaf->lifted(c);
        atomic_fetch_add(&leaf->calls, 1);
        return;
    }
    const uint32_t fpsr_cum = FPSR_CUM;
    uint32_t fpsr_saved = 0;
    if (leaf->clear_fpsr) {
        fpsr_saved = c->fpsr & fpsr_cum;
        c->fpsr &= ~fpsr_cum;
    }
    Ctx pristine, trial;
    memcpy(&pristine, c, sizeof pristine);
    static __thread RecCall *buffer;   /* kept between calls: a fresh allocation per call would cost more than the compare */
    static __thread unsigned buffer_cap;
    Recording rec;
    rec.calls = buffer;
    rec.cap = buffer_cap;
    rec.count = rec.next = 0;
    rec.bad = 0;
    rec.hide_callee_fpsr = leaf->clear_fpsr;
    rec.raised = 0;
    tl_rec = &rec;
    tl_depth = 0;
    leaf->lifted(c);   /* live memory sees only this run */
    tl_rec = 0;
    buffer = rec.calls;
    buffer_cap = rec.cap;
    atomic_fetch_add(&leaf->calls, 1);
    memcpy(&trial, &pristine, sizeof trial);
    tl_replay = &rec;
    leaf->rust_regs(trial.x);
    tl_replay = 0;
    int unused = rec.next != rec.count;
    int journal_bad = check_store_journal(leaf);
    int differs = unused || rec.bad || journal_bad || memcmp(&trial, c, sizeof trial);
    if (differs) {
        atomic_fetch_add(&leaf->differences, 1);
        rt_log("decomp mismatch addr=%#llx replay: recorded=%u used=%u call_mismatch=%d", (unsigned long long)leaf->address,
               rec.count, rec.next, rec.bad);
        const uint64_t *a = (const uint64_t *)(const void *)c, *b = (const uint64_t *)(const void *)&trial;
        unsigned shown = 0;
        for (unsigned i = 0; i < sizeof(Ctx) / 8 && shown < 6; ++i)
            if (a[i] != b[i]) {
                rt_log("  ctx word %u (%s): lifted=%#llx rust=%#llx", i, i < 32 ? "x" : i < 96 ? "v" : "other",
                       (unsigned long long)a[i], (unsigned long long)b[i]);
                ++shown;
            }
    }
    c->fpsr |= fpsr_saved | rec.raised;
}

static void compare(Leaf *leaf, Ctx *c) {
    if (leaf->replay) {
        compare_replay(leaf, c);
        return;
    }
    if (is_store(leaf)) {
        compare_store(leaf, c);
        return;
    }
    Ctx trial, pristine;
    memcpy(&trial, c, sizeof trial);
    if (leaf->load_kind) memcpy(&pristine, c, sizeof pristine);
    run_rust(leaf, &trial);
    leaf->lifted(c);
    if (memcmp(&trial, c, sizeof trial) && leaf->load_kind) {
        /* A getter reads live memory twice (Rust, then lifted C); another guest thread can write the field in
         * between (seen once in 77,477 calls of one `ldr w0, [x0, #0x68]`, task-50). A loader has no side
         * effect, so run the same pair again from the same input registers: a real difference repeats, a race
         * does not. A race is logged and counted on its own, never hidden; a repeat is a mismatch as before. */
        Ctx again, lifted_again;
        memcpy(&again, &pristine, sizeof again);
        memcpy(&lifted_again, &pristine, sizeof lifted_again);
        run_rust(leaf, &again);
        leaf->lifted(&lifted_again);
        if (!memcmp(&again, &lifted_again, sizeof again)) {
            atomic_fetch_add(&leaf->raced, 1);
            rt_log("decomp raced addr=%#llx (first pair differed, retry agrees)", (unsigned long long)leaf->address);
            atomic_fetch_add(&leaf->calls, 1);
            return;
        }
    }
    if (memcmp(&trial, c, sizeof trial)) {
        atomic_fetch_add(&leaf->differences, 1);
        const unsigned char *a = (const unsigned char *)c, *b = (const unsigned char *)&trial;
        size_t i = 0;
        while (a[i] == b[i]) ++i;
        rt_log("decomp mismatch addr=%#llx ctx_byte=%zu lifted=%02x rust=%02x",
               (unsigned long long)leaf->address, i, a[i], b[i]);
    }
    atomic_fetch_add(&leaf->calls, 1);
}

/* task-61: live damage_calc calls by the target's character id (0x5dd, 0x5ec, 0x615, 0x653 run the four enemy rules; the fifth counter is every other id) */
static _Atomic unsigned long long rule_calls[5];
static int rule_count_live;   /* set once the startup selftest is over: its registers hold random words */

static void note_damage_calc(const Ctx *c) {
    const uint64_t executer = c->x[0];
    if (!rule_count_live || !executer || tl_stub || tl_rec || tl_replay) return;
    const uint64_t target = *(const uint64_t *)(uintptr_t)(executer + 0x28);
    if (!target) return;
    static const uint32_t ids[4] = {0x5dd, 0x5ec, 0x615, 0x653};
    const uint32_t id = *(const uint32_t *)(uintptr_t)(target + 0x20);
    unsigned slot = 4;
    for (unsigned i = 0; i < 4; ++i)
        if (ids[i] == id) slot = i;
    atomic_fetch_add(&rule_calls[slot], 1);
}

static inline void dispatch(Leaf *leaf, Ctx *c) {
    if (__builtin_expect(leaf->address == 0x71021ff580ULL, 0)) note_damage_calc(c);
    if (leaf->probe) {   /* traced row: the lifted body runs unchanged; the call is counted and, for a replay row, recorded */
        if (mode == AB) atomic_fetch_add(&leaf->calls, 1);
        Recording *r = tl_rec;
        if (r && tl_depth == 0) {
            if (r->count >= r->cap) {
                if (r->cap >= REC_LIMIT) {
                    rt_log("decomp replay: more than %u traced calls in one body", REC_LIMIT);
                    rt_exit(10, "decomp replay: recorder full");
                }
                unsigned cap = r->cap ? r->cap * 2 : REC_START;
                RecCall *grown = realloc(r->calls, (size_t)cap * sizeof *grown);
                if (!grown) rt_exit(10, "decomp replay: out of memory growing the recorder");
                r->calls = grown;
                r->cap = cap;
            }
            {
                RecCall *call = &r->calls[r->count];
                call->target = leaf->address;
                memcpy(&call->pre, c, sizeof *c);
                if (leaf->out_bytes) memcpy(call->out_pre, (const void *)(uintptr_t)(c->x[leaf->out_reg] + leaf->out_off), leaf->out_bytes);
                /* The flags the callee raises are kept out of the context while the compared body runs, so that a
                 * flag the port forgets after the call cannot hide behind one the callee already set (task-54).
                 * They are ORed back once the comparison is done (compare_replay). */
                uint32_t held = 0;
                if (r->hide_callee_fpsr) { held = c->fpsr & FPSR_CUM; c->fpsr &= ~FPSR_CUM; }
                ++tl_depth;
                run_traced(leaf, c);
                --tl_depth;
                if (r->hide_callee_fpsr) { r->raised |= c->fpsr & FPSR_CUM; c->fpsr = (c->fpsr & ~FPSR_CUM) | held; }
                memcpy(&call->post, c, sizeof *c);
                if (leaf->out_bytes) memcpy(call->out_post, (const void *)(uintptr_t)(call->pre.x[leaf->out_reg] + leaf->out_off), leaf->out_bytes);
                ++r->count;
            }
        } else if (r) {   /* nested inside a recorded callee */
            ++tl_depth;
            run_traced(leaf, c);
            --tl_depth;
        } else {
            run_traced(leaf, c);
        }
        return;
    }
    if (mode == OFF) leaf->lifted(c);
    else if (mode == ON) {
        if (leaf->rust_regs) atomic_fetch_add(&leaf->calls, 1);   /* task-58: live counts of the replay and bridge rows in `on` mode (evidence the route reached them) */
        run_rust(leaf, c);
    } else compare(leaf, c);
}

#define LEAF(chunk, addr, rust) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define SLEAF(chunk, addr, rust, base, off, len) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define CLEAF(chunk, addr, rust) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define GLEAF(chunk, addr, rust, base, off, len) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define NLEAF(chunk, addr, rust, base, off, len) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define DLEAF(chunk, addr, rust, base, off, len, off2) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define VLEAF(chunk, addr, rust, vkind, vreg, base, off, len, off2) \
    void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define PLEAF(chunk, addr) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define OLEAF(chunk, addr, reg, off, bytes, in) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define ELEAF(chunk, addr, rust) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define QLEAF(chunk, addr, rust, hook) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#include "manifest.def"
#undef QLEAF
#undef ELEAF
#undef OLEAF
#undef PLEAF
#undef LEAF
#undef SLEAF
#undef CLEAF
#undef GLEAF
#undef NLEAF
#undef DLEAF
#undef VLEAF

/* ---- Guest-call bridge (task-52) ----
 * A readable Rust port calls a function that is still lifted C through smrpg_guest_bl (decomp/smrpg_decomp/src/
 * bridge.rs). It does what a lifted `bl` does: the callee runs on the caller's own register file (x0..x7 staged,
 * x30 = the return address the lifted code would have written, sp unchanged, so the callee's stack frame sits
 * where the lifted code's would, below the caller's sp), and the callee's registers stay in that file as residue.
 * A direct call to the registered entry, like the generated `f_<addr>(c)`; an address with no lifted entry goes
 * to the interpreter fallback (rt_icall). Returns 1 when the callee left a C++ exception pending (the lifted
 * caller returns at once in that case, and so must the port).
 *
 * ab rule. A port compared in `ab` mode runs on a copy of the context while the lifted caller then runs on the
 * live one, so every callee runs twice on the same live memory. That is only allowed for a callee that cannot
 * change memory: the pure allowlist below (pure.def, each entry proven from the lifted chunk by check_pure.py:
 * stores only inside its own stack frame, no indirect call, no atomic, every callee pure in turn) plus the ported
 * load rows (LEAF, GLEAF, NLEAF, DLEAF, VLEAF loads and constants). Any other target in `ab` mode stops the run
 * (exit 10) instead of running a side effect twice. `on` mode runs the port once, so it has no such limit. */
_Static_assert(offsetof(Ctx, v) == 256, "decomp bridge: Rust reads v0..v31 at x + 32 words");
_Static_assert(offsetof(Ctx, fpcr) == 780 && offsetof(Ctx, fpsr) == 784, "decomp bridge: Rust reads FPCR and FPSR at byte 780 and 784 (task-53)");

static const uint64_t pure_callees[] = {
#define PURE_FN(addr) 0x##addr##ULL,
#include "pure.def"
#undef PURE_FN
};

static int callee_is_pure(uint64_t target) {
    for (int i = 0; i < RT_ARRAY_LEN(pure_callees); ++i)
        if (pure_callees[i] == target) return 1;
    for (int i = 0; i < leaf_count; ++i)
        if (leaves[i].address == target) return leaves[i].rust || (leaves[i].rust_vec && leaves[i].vkind != 2);
    return 0;
}

extern __thread int g_rt_pure;

/* Replay (ELEAF rows): the call is not executed, it is matched with the next recorded one. */
static int replay_call(Recording *r, Ctx *c, uint64_t target) {
    if (r->next >= r->count) {
        r->bad = 1;
        rt_log("decomp replay: the port made an extra call to %#llx (recorded %u)", (unsigned long long)target, r->count);
        return c->exc != 0;
    }
    RecCall *call = &r->calls[r->next++];
    if (call->target != target) {
        r->bad = 1;
        rt_log("decomp replay: call %u went to %#llx in the port, %#llx in the lifted body", r->next, (unsigned long long)target,
               (unsigned long long)call->target);
    } else {
        unsigned shown = 0;
        for (unsigned i = 0; i < 32; ++i)
            if (c->x[i] != call->pre.x[i] && shown++ < 8) {
                r->bad = 1;
                rt_log("decomp replay: call %u to %#llx, x%u: lifted=%#llx rust=%#llx", r->next, (unsigned long long)target, i,
                       (unsigned long long)call->pre.x[i], (unsigned long long)c->x[i]);
            }
        if (shown && !r->dumped) { /* task-60: the first mismatch of a trial lists the recorded calls (target, x29, sp) to find which site differs */
            r->dumped = 1;
            for (unsigned k = 0; k < r->count && k < 40; ++k) rt_log("DBG call %u target=%#llx x29=%#llx x31=%#llx", k + 1, (unsigned long long)r->calls[k].target, (unsigned long long)r->calls[k].pre.x[29], (unsigned long long)r->calls[k].pre.x[31]);
        }
        for (unsigned i = 0; i < 32; ++i)
            if (memcmp(&c->v[i], &call->pre.v[i], sizeof c->v[i]) && shown++ < 8) {
                r->bad = 1;
                rt_log("decomp replay: call %u to %#llx, v%u differs", r->next, (unsigned long long)target, i);
            }
        if ((c->fpcr != call->pre.fpcr || c->fpsr != call->pre.fpsr) && shown++ < 8) {
            r->bad = 1;
            rt_log("decomp replay: call %u to %#llx, fpcr/fpsr differ", r->next, (unsigned long long)target);
        }
    }
    memcpy(c, &call->post, sizeof *c);
    return c->exc != 0;
}

/* task-60: the exception test the lifted code makes after a call, for a nested port (it has no return value). */
int smrpg_guest_exc(uint64_t *x) {
    return ((Ctx *)(void *)x)->exc != 0;
}

/* task-60: a port that leaves a case to the lifted body (the damage_calc driver for a few character ids) runs it here. Only
 * `on` mode may: in a replay trial the lifted body already ran on live memory, so running it again would repeat its side effects. */
void smrpg_guest_lifted(uint64_t *x, uint64_t target) {
    if (tl_replay) rt_exit(10, "decomp bridge: lifted body requested inside a replay trial");
    for (int i = 0; i < leaf_count; ++i)
        if (leaves[i].address == target && leaves[i].lifted) {
            leaves[i].lifted((Ctx *)(void *)x);
            return;
        }
    rt_exit(10, "decomp bridge: no lifted body for the requested address");
}

int smrpg_guest_bl(uint64_t *x, uint64_t target) {
    Ctx *c = (Ctx *)(void *)x;   /* Ctx.x is the first member: the Rust side holds a pointer to the register file */
    if (tl_replay) {
        for (int i = 0; i < leaf_count; ++i)
            if (leaves[i].probe && leaves[i].address == target) return replay_call(tl_replay, c, target);
    }
    if (mode == AB && !callee_is_pure(target)) {
        rt_log("decomp bridge: ab mode, callee %#llx is not on the pure allowlist (it would run twice)", (unsigned long long)target);
        rt_exit(10, "decomp bridge: callee not pure");
    }
    GuestFn f = g_rt_pure ? 0 : rt_lookup(target);
    if (f) f(c);
    else rt_icall(c, target);   /* no lifted entry: interpreter fallback, which also sets c->pc like a miss */
    return c->exc != 0;
}

/* Guest call with an out footprint (task-57, OLEAF rows). The callee writes `n` bytes at x[out_reg] + out_off: a slot of
 * the caller's own stack frame, or fields of an object the caller edits in place (the header of a List<T>). The port
 * keeps those bytes as a local buffer, so it hands the buffer over: `buf` holds the bytes the area has at the call and
 * receives the bytes it has after.
 *  - replay trial: the callee does not run. The call is matched as usual (smrpg_guest_bl). For a row that declares the
 *    area an input too (out_in) the port's bytes must equal the bytes the lifted body had there at the call, so a wrong
 *    value passed by reference, or a store the port made to the area before the call, is checked here; then `buf` gets
 *    the bytes the callee left, recorded from the lifted run. The area now holds what the callee changed, so the
 *    journal no longer compares the port's earlier stores into it with the final memory (they were just compared with
 *    the memory at the call): those entries and the watch marks on those cells are dropped.
 *  - otherwise the port is the only run (on mode): the bytes go to the area (input rows only), the callee runs on the
 *    live stack, and `buf` receives what it wrote. A row that is not declared with this footprint stops the run. */
int smrpg_guest_bl_out(uint64_t *x, uint64_t target, unsigned char *buf, unsigned n) {
    Ctx *c = (Ctx *)(void *)x;
    const Leaf *row = 0;
    for (int i = 0; i < leaf_count; ++i)
        if (leaves[i].probe && leaves[i].address == target) row = &leaves[i];
    if (!row || row->out_bytes != n || n > OUT_MAX) rt_exit(10, "decomp bridge: callee has no matching out footprint row");
    if (tl_replay) {
        Recording *r = tl_replay;
        const unsigned index = r->next;
        const int pending = replay_call(r, c, target);
        if (index < r->count && r->calls[index].target == target) {
            const RecCall *call = &r->calls[index];
            const uint64_t lo = call->pre.x[row->out_reg] + row->out_off, hi = lo + n;
            if (row->out_in && memcmp(buf, call->out_pre, n)) {
                r->bad = 1;
                rt_log("decomp replay: call %u to %#llx, the bytes passed by reference differ from the lifted body's", index + 1,
                       (unsigned long long)target);
            }
            memcpy(buf, call->out_post, n);
            unsigned kept = 0;
            for (unsigned i = 0; i < tl_st_n; ++i)
                if (tl_st[i].addr < lo || tl_st[i].addr >= hi) tl_st[kept++] = tl_st[i];
            tl_st_n = kept;
            /* task-60: what the callee left in the cell is a store of its own: a later read of the cell by the port or a nested port
             * sees it, and the end-of-trial check holds it to the lifted body's final memory (unless a later store supersedes it) */
            if ((n == 1 || n == 4 || n == 8) && tl_st_n < PRE_MAX) {
                uint64_t post = 0;
                memcpy(&post, call->out_post, n);
                tl_st[tl_st_n++] = (StoreRec){lo, n, post};
            }
            for (unsigned i = 0; i < tl_pre_n; ++i)
                if (tl_pre[i].addr >= lo && tl_pre[i].addr < hi) tl_pre[i].watch = 0;
        }
        return pending;
    }
    unsigned char *const area = (unsigned char *)(uintptr_t)(c->x[row->out_reg] + row->out_off);
    if (row->out_in) memcpy(area, buf, n);
    const int pending = smrpg_guest_bl(x, target);
    memcpy(buf, area, n);
    return pending;
}

/* The Rust archive is built for a Windows msvc target, where any float use references the CRT marker `_fltused`
 * (first float code in the archive: fp.rs and the camera port, task-53). This build links no msvc CRT, so define it. */
int _fltused = 0;

/* The lifted loop back-edge safepoint (task-53, bridge.rs Regs::safepoint): with a collection pending it flushes the
 * register the lifted code flushes at that point, records the pc and polls; otherwise it does nothing, exactly as
 * `if (g_gc_pending != 0) { c->x[reg] = ...; c->pc = pc; gc_poll(c); }` does. A replay trial (the lifted body already
 * ran and polled) only imitates the context change. */
void smrpg_guest_safepoint(uint64_t *x, uint64_t pc, uint32_t reg, uint64_t value) {
    Ctx *c = (Ctx *)(void *)x;
    if (__builtin_expect(g_gc_pending != 0, 0)) {
        c->x[reg] = value;
        c->pc = pc;
        if (!tl_replay) gc_poll(c);
    }
}

/* The same safepoint where the lifted code flushes several registers there (task-55: get_calc_attack flushes two). */
void smrpg_guest_safepoint_n(uint64_t *x, uint64_t pc, uint32_t count, const uint32_t *regs, const uint64_t *values) {
    Ctx *c = (Ctx *)(void *)x;
    if (__builtin_expect(g_gc_pending != 0, 0)) {
        for (uint32_t i = 0; i < count; ++i) c->x[regs[i]] = values[i];
        c->pc = pc;
        if (!tl_replay) gc_poll(c);
    }
}

/* The same where the lifted code also flushes vector registers (task-58: calc_renkei_ratio's loop poll flushes v0 and v1). */
void smrpg_guest_safepoint_nv(uint64_t *x, uint64_t pc, uint32_t count, const uint32_t *regs, const uint64_t *values, uint32_t vcount,
                              const uint32_t *vregs, const uint64_t *vlow, const uint64_t *vhigh) {
    Ctx *c = (Ctx *)(void *)x;
    if (__builtin_expect(g_gc_pending != 0, 0)) {
        for (uint32_t i = 0; i < count; ++i) c->x[regs[i]] = values[i];
        for (uint32_t i = 0; i < vcount; ++i) { c->v[vregs[i]][0] = (long long)vlow[i]; c->v[vregs[i]][1] = (long long)vhigh[i]; }
        c->pc = pc;
        if (!tl_replay) gc_poll(c);
    }
}

static uint64_t random_word(uint64_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

/* Leaves whose x0 is a guest pointer that they dereference. The selftest points x0 into a private fake
 * region (or at null) so it needs no game data. Leaves that read a fixed global slot (task-42) read the
 * real mapped slot, which both the Rust and lifted bodies see identically. */
static int reads_x0_pointer(uint64_t address) {
    return address == 0x71005b3a20ULL || address == 0x710200fd30ULL;
}

static void fill_fake_region(uint64_t *words, unsigned count, uint64_t *state) {
    for (unsigned i = 0; i < count; ++i) {
        uint64_t r = random_word(state);
        switch (r & 3) {   /* zero lengths, small values and full-width values all matter */
        case 0: words[i] = 0; break;
        case 1: words[i] = (r >> 60) | ((r & 4) ? 0x100000000ULL : 0); break;
        default: words[i] = r; break;
        }
    }
}

/* Readable ports of Battle classes (task-51, decomp/smrpg_decomp/src/battle.rs). Each takes an object graph:
 * motion (+0x10) -> character (+0x20) -> param (+0x98 status manager, +0xa0 buff manager). The selftest builds
 * the whole graph in the private fake region with edge values for every field the ports read (never game
 * memory). Returns which object x0 is: 1 param, 2 status manager, 3 Mario motion special, 0 not a battle port. */
static int battle_object_kind(uint64_t address) {
    switch (address) {
    case 0x71021c3bb0ULL: case 0x71021bdec0ULL: case 0x71021bcb30ULL: case 0x71021c3540ULL: case 0x71021c3600ULL:
    case 0x71021c3580ULL: case 0x71021c3640ULL: case 0x71021c35a0ULL: return 1;
    case 0x7101e2fa80ULL: return 2;
    case 0x7101e73c40ULL: return 3;
    default: return 0;
    }
}

static uint32_t battle_edge_word(uint64_t *state) {
    static const uint32_t edges[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0xffffffffu, 0x80000000u, 0x7fffffffu};
    uint64_t r = random_word(state);
    return (r & 3) == 0 ? (uint32_t)(r >> 8) : edges[(r >> 8) % RT_ARRAY_LEN(edges)];
}

static void prepare_battle(int kind, Ctx *c, uint64_t *fake, uint64_t *state) {
    fill_fake_region(fake, 64, state);
    const uint64_t base = (uint64_t)(uintptr_t)fake;
    unsigned char *param = (unsigned char *)fake;                 /* word 0: BattleCharacterParam (0xb0 bytes) */
    unsigned char *status = (unsigned char *)&fake[24];           /* BadStatusManager */
    unsigned char *buff = (unsigned char *)&fake[32];             /* BuffManager */
    uint64_t *character = &fake[40];                              /* BattleCharacter, +0x20 = param */
    uint64_t *motion = &fake[48];                                 /* MotionSpecial_Mario, +0x10 = character */
    static const unsigned bools[] = {0x6e, 0x71, 0x78};           /* is_join, is_bunnage, can_targetting */
    static const unsigned char truthy[] = {1, 1, 1, 2, 0x80, 0xff};  /* any non-zero byte is true */
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t r = random_word(state);
        unsigned on_in_eight = i == 1 ? 2 : 7;                    /* join and targetting lean true, bunnage false */
        param[bools[i]] = (r & 7) < on_in_eight ? truthy[(r >> 8) % RT_ARRAY_LEN(truthy)] : 0;
    }
    uint32_t act = battle_edge_word(state), turn = battle_edge_word(state), kind_word = battle_edge_word(state);
    memcpy(param + 0x5c, &act, 4);
    memcpy(buff + 0x28, &turn, 4);
    uint32_t barriers = battle_edge_word(state);                  /* is_barriered (task-56): _barrier_num at +0x2c */
    memcpy(buff + 0x2c, &barriers, 4);
    memcpy(status + 0x10, &kind_word, 4);
    memcpy(param + 0x98, &(uint64_t){base + 8 * 24}, 8);
    memcpy(param + 0xa0, &(uint64_t){base + 8 * 32}, 8);
    character[4] = base;                                          /* +0x20 */
    uint64_t pick = random_word(state);
    motion[2] = (pick & 7) == 0 ? 0 : base + 8 * 40;              /* +0x10: null one time in eight */
    c->x[0] = kind == 1 ? base : kind == 2 ? base + 8 * 24 : base + 8 * 48;
}

/* get_calc_attack and get_calc_magic_attack read game globals (the flag bytes, three metadata slots). The selftest
 * (below, prepare_caller) points them into a private region and puts the originals back afterwards. */
static uint64_t attack_saved[5];
static int attack_have_saved;
static void restore_attack_globals(void) {
    if (!attack_have_saved) return;
    *(volatile uint8_t *)(uintptr_t)0x7103cbac09ULL = (uint8_t)attack_saved[0];
    *(volatile uint8_t *)(uintptr_t)0x7103cbac0bULL = (uint8_t)attack_saved[1];
    *(uint64_t *)(uintptr_t)0x7103a59328ULL = attack_saved[2];
    *(uint64_t *)(uintptr_t)0x7103a57e48ULL = attack_saved[3];
    *(uint64_t *)(uintptr_t)0x7103a581d0ULL = attack_saved[4];
    attack_have_saved = 0;
}

/* calc_belome_barrior_ratio (task-57) reads the lazy-initialization flag and three metadata slots; the selftest points them
 * into a private region and puts the originals back afterwards. */
static uint64_t belome_saved[4];
static int belome_have_saved;
/* calc_renkei_ratio (task-58): the flag byte and the 13 metadata slots (data page + offset), restored after its selftest. */
static const uint64_t renkei_slot_address[13] = {
    0x7103a4d000ULL + 0x788, 0x7103a4d000ULL + 0x700, 0x7103a4d000ULL + 0x6f8, 0x7103a46000ULL + 0xa90,
    0x7103a83000ULL + 0x520, 0x7103a83000ULL + 0x528, 0x7103a83000ULL + 0x530, 0x7103a83000ULL + 0x538, 0x7103a83000ULL + 0x540,
    0x7103a83000ULL + 0x548, 0x7103a83000ULL + 0x550, 0x7103a83000ULL + 0x558, 0x7103a83000ULL + 0x560};
static uint64_t renkei_saved[14];
static int renkei_have_saved;
static void restore_renkei_globals(void) {
    if (!renkei_have_saved) return;
    *(volatile uint8_t *)(uintptr_t)(0x7103cba000ULL + 0xd5e) = (uint8_t)renkei_saved[0];
    for (int i = 0; i < 13; ++i) *(uint64_t *)(uintptr_t)renkei_slot_address[i] = renkei_saved[1 + i];
    renkei_have_saved = 0;
    tl_stub_new = 0;
}

/* group_a_b (task-59): the flag byte and nine metadata slots (data page + offset), restored after its selftest. */
static const uint64_t group_slot_address[9] = {
    0x7103a57000ULL + 0xe48, 0x7103a58000ULL + 0x1d0, 0x7103a58000ULL + 0x240, 0x7103a58000ULL + 0x248, 0x7103a58000ULL + 0x250,
    0x7103a58000ULL + 0x258, 0x7103a83000ULL + 0x540, 0x7103a83000ULL + 0xc28, 0x7103a83000ULL + 0xc30};
static uint64_t group_saved[10];
static int group_have_saved;
static void restore_group_globals(void) {
    if (!group_have_saved) return;
    *(volatile uint8_t *)(uintptr_t)(0x7103cba000ULL + 0xd73) = (uint8_t)group_saved[0];
    for (int i = 0; i < 9; ++i) *(uint64_t *)(uintptr_t)group_slot_address[i] = group_saved[1 + i];
    group_have_saved = 0;
    tl_stub_current = 0;
}

/* damage_calc driver (task-60): its own flag byte and 26 metadata slots plus everything its nested ports read (group_a_b,
 * calc_renkei_ratio, calc_belome_barrior_ratio: flag bytes and slots), saved once and restored after the row. */
static const uint64_t driver_slot_address[26] = {
    0x7103a57e48ULL, 0x7103a58048ULL, 0x7103a4d788ULL, 0x7103a4e8c8ULL, 0x7103a4d790ULL, 0x7103a46a90ULL, 0x7103a83b48ULL, 0x7103a83b50ULL,
    0x7103a83b58ULL, 0x7103a83540ULL, 0x7103a83b60ULL, 0x7103a83b68ULL, 0x7103a83b70ULL, 0x7103a83b78ULL, 0x7103a83b80ULL, 0x7103a83b88ULL,
    0x7103a83b90ULL, 0x7103a83b98ULL, 0x7103a83ba0ULL, 0x7103a83ba8ULL, 0x7103a83bb0ULL, 0x7103a83b40ULL, 0x7103a83bb8ULL, 0x7103a835b0ULL,
    0x7103a83bc0ULL, 0x7103a83bc8ULL};
static const uint64_t driver_flag_address[4] = {0x7103cba000ULL + 0xd71, 0x7103cba000ULL + 0xd73, 0x7103cba000ULL + 0xd5e, 0x7103cba000ULL + 0xd5d};
#define DRIVER_SLOTS (26 + 9 + 13)
static uint64_t driver_saved[DRIVER_SLOTS + 4];
static int driver_have_saved;
static uint64_t driver_slot_at(int i) {
    if (i < 26) return driver_slot_address[i];
    if (i < 35) return group_slot_address[i - 26];
    return renkei_slot_address[i - 35];
}
static void restore_driver_globals(void) {
    if (!driver_have_saved) return;
    for (int i = 0; i < 4; ++i) *(volatile uint8_t *)(uintptr_t)driver_flag_address[i] = (uint8_t)driver_saved[DRIVER_SLOTS + i];
    for (int i = 0; i < DRIVER_SLOTS; ++i) *(uint64_t *)(uintptr_t)driver_slot_at(i) = driver_saved[i];
    driver_have_saved = 0;
    tl_stub_instance = tl_stub_new = tl_stub_current = 0;
}

static void restore_belome_globals(void) {
    if (!belome_have_saved) return;
    *(volatile uint8_t *)(uintptr_t)(0x7103cba000ULL + 0xd5d) = (uint8_t)belome_saved[0];
    *(uint64_t *)(uintptr_t)(0x7103a58000ULL + 0x48) = belome_saved[1];
    *(uint64_t *)(uintptr_t)(0x7103a4d000ULL + 0x788) = belome_saved[2];
    *(uint64_t *)(uintptr_t)(0x7103a4e000ULL + 0x8c8) = belome_saved[3];
    belome_have_saved = 0;
    tl_stub_instance = 0;
}

/* Caller functions (task-44) dereference pointers they compute, so the selftest builds a valid shape in the
 * private fake region for each one (never game memory). Register-only callers keep the random registers. */
static void prepare_caller(uint64_t address, Ctx *c, uint64_t *fake, uint64_t *state, int edge_phase) {
    uint64_t pick = random_word(state);
    uint64_t base = (uint64_t)(uintptr_t)fake;
    /* The lifted prologue pushes x29 and x30 below sp, so sp must be a real 16-byte aligned stack. The Rust
     * body has no stack; those bytes are dead once the function returns and are not compared. */
    static _Alignas(16) uint64_t stack[128];   /* task-58: calc_renkei_ratio's frame is 0x70 bytes, the old 16 words overflowed it */
    c->x[31] = (uint64_t)(uintptr_t)&stack[104];
    switch (address) {
    case 0x71005b3d70ULL:   /* x0 + (int)x1 * 8 + 0x20 is loaded: keep it inside the 64 words */
        fill_fake_region(fake, 64, state);
        c->x[0] = base + 8 * (8 + (pick & 15));
        c->x[1] = (random_word(state) & ~0xffffffffULL) | (uint32_t)(int32_t)((int)((pick >> 8) % 20) - 4);
        break;
    case 0x71005ae5a0ULL: { /* null, or object -> field (null or in range) -> u32 at field + 8 */
        fill_fake_region(fake, 64, state);
        unsigned object = (pick >> 8) % 20;
        if ((pick & 7) == 0) c->x[0] = 0;
        else {
            c->x[0] = base + 8 * object;
            fake[object + 2] = (pick & 8) ? 0 : base + 8 * (32 + ((pick >> 16) % 28));
        }
        break;
    }
    case 0x7101cccaf0ULL:   /* w1 at, around and far from the 0x81d compare and the 0x7f8..0x7fb range */
        if (!edge_phase) {
            static const uint32_t edge_w1[] = {0x81d, 0x7f7, 0x7f8, 0x7f9, 0x7fa, 0x7fb, 0x7fc, 0x81c, 0x81e};
            if (pick % 3) c->x[1] = (c->x[1] & ~0xffffffffULL) | edge_w1[(pick >> 8) % RT_ARRAY_LEN(edge_w1)];
        }
        break;
    case 0x71001919f0ULL:   /* x0 -> object whose +0x10 word is compared with x1 (equal about half the time) */
        fill_fake_region(fake, 64, state);
        c->x[0] = base + 8 * ((pick >> 8) % 40);
        if (pick & 1) c->x[1] = *(uint64_t *)(uintptr_t)(c->x[0] + 0x10);
        break;
    case 0x7101b8abd0ULL: {   /* bridge selftest row (task-52): x0 = A, A+0x10 -> B, B+0x53 flag bits, B+0x40 -> P, P+0x10 read */
        fill_fake_region(fake, 64, state);
        fake[2] = base + 8 * 8;                                    /* A+0x10 = B (B is word 8) */
        ((unsigned char *)&fake[8])[0x53] = (unsigned char)((pick >> 8) & 3) | ((pick & 0x400) ? 0x80 : 0);   /* bit 0, bit 1, both, neither */
        fake[16] = base + 8 * (20 + ((pick >> 16) & 7));           /* B+0x40 = P, never null (the callee adds 8 and reads +8) */
        c->x[0] = base;
        break;
    }
    case 0x7101cf7650ULL: { /* Battle.BattleHit.exec_hit (task-52): +0x20 target, +0x28 skill params, +0x81 flag; target +0x18 motion */
        fill_fake_region(fake, 64, state);
        const uint64_t target = base + 8 * 20, motion = base + 8 * 44;
        fake[4] = (pick & 7) == 0 ? 0 : target;                      /* _target, null one time in eight */
        fake[5] = ((pick >> 3) & 7) == 0 ? 0 : base + 8 * 40;       /* _skill_params */
        static const unsigned char flags[] = {0, 0, 0, 1, 2, 0x80, 0xff};
        ((unsigned char *)&fake[16])[1] = flags[(pick >> 8) % RT_ARRAY_LEN(flags)];   /* _is_only_se_hit at +0x81 */
        fake[23] = ((pick >> 16) & 3) == 0 ? 0 : motion;            /* target._motion at +0x18 */
        c->x[0] = base;
        break;
    }
    case 0x7101e2cfd0ULL: { /* Battle.BattleCamera.adjust_camera_transform (task-53): floats at +0x3c, +0x48, +0x54; flag +0x74;
                             * shake object at +0x90 with its offset at +0x34. Float edges and every FPCR mode. */
        fill_fake_region(fake, 64, state);
        static const uint32_t edge_floats[] = {
            0, 0x80000000u, 1, 0x80000001u, 0x007fffffu, 0x00800000u, 0x3f800000u, 0xbf800000u, 0x3f800001u, 0x33800000u,
            0x4b000000u, 0x42b40000u, 0xc2b40000u, 0x43b40000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u,
            0x7fc00000u, 0xffc01234u, 0x7f800001u, 0x7fbfffffu, 0x3c8efa35u, 0x3eaaaaabu};
        unsigned char *bytes = (unsigned char *)fake;
        static const unsigned float_fields[] = {0x3c, 0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 8 * 32 + 0x34, 8 * 32 + 0x38, 8 * 32 + 0x3c};
        for (unsigned i = 0; i < RT_ARRAY_LEN(float_fields); ++i) {
            uint64_t r = random_word(state);
            uint32_t value = (r & 3) == 0 ? (uint32_t)(r >> 8) : edge_floats[(r >> 8) % RT_ARRAY_LEN(edge_floats)];
            memcpy(bytes + float_fields[i], &value, 4);
        }
        static const unsigned char flags[] = {0, 0, 1, 2, 0x80, 0xff};
        bytes[0x74] = flags[(pick >> 8) % RT_ARRAY_LEN(flags)];
        fake[0x90 / 8] = ((pick >> 16) & 3) == 0 ? 0 : base + 8 * 32;   /* _camera_shake, null one time in four */
        fake[2] = random_word(state);                                   /* _camera: passed on, never read */
        c->x[0] = base;
        static const uint32_t fpcr_modes[] = {0, 0, 0, 1u << 24, 1u << 25, (1u << 24) | (1u << 25), 1u << 22, 2u << 22, 3u << 22};
        c->fpcr = fpcr_modes[(pick >> 24) % RT_ARRAY_LEN(fpcr_modes)];
        c->fpsr = (uint32_t)(pick >> 32) & 0x0800009fu;
        break;
    }
    case 0x71021c3ea0ULL:   /* Battle.BattleCharacterParam get_calc_attack and get_calc_magic_attack (task-55). Besides the object graph they */
    case 0x71021bc2e0ULL: { /* read game globals (flag byte, three metadata slots), which the selftest points into a private region and restores. */
        static uint64_t region[192];
        volatile uint8_t *flag_physical = (volatile uint8_t *)(uintptr_t)0x7103cbac09ULL, *flag_magic = (volatile uint8_t *)(uintptr_t)0x7103cbac0bULL;
        uint64_t *slot_strawberry = (uint64_t *)(uintptr_t)0x7103a59328ULL, *slot_manager = (uint64_t *)(uintptr_t)0x7103a57e48ULL,
                 *slot_flag = (uint64_t *)(uintptr_t)0x7103a581d0ULL;
        if (!attack_have_saved) {
            attack_saved[0] = *flag_physical; attack_saved[1] = *flag_magic; attack_saved[2] = *slot_strawberry;
            attack_saved[3] = *slot_manager; attack_saved[4] = *slot_flag;
            attack_have_saved = 1;
        }
        fill_fake_region(region, 192, state);
        unsigned char *bytes = (unsigned char *)region;
        const uint64_t rb = (uint64_t)(uintptr_t)region;
        static const uint32_t edge_ints[] = {0, 1, 0xffffffffu, 100, 0xffffff9cu, 0x00ffffffu, 0x7fffffffu, 0x80000000u, 0x01234567u, 5, 0x2aaaaaabu};
        static const uint32_t lengths[] = {0, 1, 2, 3, 4, 5, 9, 0xffffffffu};
        static const unsigned char byte_flags[] = {0, 0, 1, 1, 2, 0x80, 0xff};
        #define EDGE_INT() (((r = random_word(state)) & 3) == 0 ? (uint32_t)(r >> 8) : edge_ints[(r >> 8) % RT_ARRAY_LEN(edge_ints)])
        uint64_t r;
        uint32_t v;
        /* the param (words 0..23): _attack 0x3c, _magic_attack 0x44, _is_player 0x6c, _buff_mng 0xa0, _weapon 0xb0 */
        v = EDGE_INT(); memcpy(bytes + 0x3c, &v, 4);
        v = EDGE_INT(); memcpy(bytes + 0x44, &v, 4);
        bytes[0x6c] = byte_flags[(pick >> 8) % RT_ARRAY_LEN(byte_flags)];
        region[0xa0 / 8] = rb + 8 * 24;                           /* buff manager */
        region[0xb0 / 8] = rb + 8 * 40;                           /* weapon */
        region[24 + 3] = rb + 8 * 32;                             /* buff + 0x18: _add_status */
        v = lengths[(pick >> 16) % RT_ARRAY_LEN(lengths)]; memcpy(bytes + 8 * 32 + 0x18, &v, 4);
        for (unsigned i = 0; i < 4; ++i) { v = EDGE_INT(); memcpy(bytes + 8 * 32 + 0x20 + 4 * i, &v, 4); }
        bytes[8 * 40 + 0x10] = byte_flags[(pick >> 24) % RT_ARRAY_LEN(byte_flags)];   /* weapon._is_exist */
        for (unsigned i = 0; i < 3; ++i) { v = EDGE_INT(); memcpy(bytes + 8 * 40 + 0x14 + 4 * i, &v, 4); }
        /* BattleManager class (word 64): initialized word at +0xe0, static fields at +0xb8 -> +0x10 -> manager (word 104), id at +0x168 */
        region[64 + 0xb8 / 8] = rb + 8 * 100;
        region[100 + 2] = rb + 8 * 104;
        v = ((pick >> 32) & 7) == 0 ? 0 : (uint32_t)(r = random_word(state)) | 1; memcpy(bytes + 8 * 64 + 0xe0, &v, 4);
        v = ((pick >> 36) & 1) ? 0x81c : (((pick >> 37) & 3) == 0 ? 0x81b : EDGE_INT()); memcpy(bytes + 8 * 104 + 0x168, &v, 4);
        /* strawberry class (word 120) and the two slot holders (words 150, 152), the static flag byte (word 154) */
        v = ((pick >> 40) & 7) == 0 ? 0 : 1; memcpy(bytes + 8 * 120 + 0xe0, &v, 4);
        region[150] = rb + 8 * 64;
        region[152] = rb + 8 * 120;
        bytes[8 * 154] = ((pick >> 44) & 3) == 0 ? 0 : (unsigned char)(1 + ((pick >> 46) & 1));
        *slot_manager = rb + 8 * 150;
        *slot_strawberry = rb + 8 * 152;
        *slot_flag = rb + 8 * 154;
        *flag_physical = ((pick >> 48) & 15) == 0 ? 0 : 1;        /* lazy initialization pending one time in sixteen */
        *flag_magic = ((pick >> 52) & 15) == 0 ? 0 : 1;
        #undef EDGE_INT
        c->x[0] = rb;
        static const uint32_t fpcr_modes[] = {0, 0, 0, 1u << 24, 1u << 25, (1u << 24) | (1u << 25), 1u << 22, 2u << 22, 3u << 22};
        c->fpcr = fpcr_modes[(pick >> 56) % RT_ARRAY_LEN(fpcr_modes)];
        c->fpsr = (uint32_t)(pick >> 32) & 0x0800009fu;
        break;
    }
    case 0x71021f5c50ULL: { /* Battle.SkillExecter.calc_belome_barrior_ratio (task-57): the executer (+0x20 acter, +0x28 target), the target's logger
                             * (+0xb8) and its List<int> (+0x50), the skill object (+0x14 id, +0x24 calc type, +0x38 element), the GlobalWork
                             * stand-in with its bool[] (+0xe8), the GlobalWork class (+0xe0), the flag byte and three metadata slots. */
        static uint64_t region[256];
        volatile uint8_t *flag = (volatile uint8_t *)(uintptr_t)(0x7103cba000ULL + 0xd5d);
        uint64_t *slot_class = (uint64_t *)(uintptr_t)(0x7103a58000ULL + 0x48), *slot_add = (uint64_t *)(uintptr_t)(0x7103a4d000ULL + 0x788),
                 *slot_context = (uint64_t *)(uintptr_t)(0x7103a4e000ULL + 0x8c8);
        if (!belome_have_saved) {
            belome_saved[0] = *flag; belome_saved[1] = *slot_class; belome_saved[2] = *slot_add; belome_saved[3] = *slot_context;
            belome_have_saved = 1;
        }
        fill_fake_region(region, 256, state);
        unsigned char *bytes = (unsigned char *)region;
        const uint64_t rb = (uint64_t)(uintptr_t)region;
        uint64_t r;
        uint32_t v;
        #define WORD(n) (rb + 8 * (n))
        region[4] = WORD(8);                                   /* executer +0x20 _acter */
        region[5] = WORD(24);                                  /* +0x28 _target */
        static const unsigned char shoes[] = {0, 0, 1, 2, 0xff};
        bytes[8 * 8 + 0x7d] = shoes[(pick >> 8) % RT_ARRAY_LEN(shoes)];
        static const uint32_t ids[] = {0x649, 0x649, 0x649, 0x649, 0x649, 0x649, 0x649, 0x648, 0, 0xffffffffu};
        v = ids[(pick >> 12) % RT_ARRAY_LEN(ids)]; memcpy(bytes + 8 * 24 + 0x20, &v, 4);   /* target _character_id */
        region[24 + 0xb8 / 8] = WORD(48);                      /* target +0xb8 _result_logger */
        region[48 + 0x50 / 8] = ((pick >> 16) & 7) == 0 ? 0 : WORD(60);   /* logger +0x50 _multi_number (null one time in eight) */
        region[62] = WORD(66);                                 /* list +0x10 _items */
        static const uint32_t sizes[] = {0, 0, 1, 2, 3, 5, 0xffffffffu, 0x80000000u};
        static const uint32_t versions[] = {0, 1, 0xffffffffu, 0x7fffffffu, 5};
        v = sizes[(pick >> 20) % RT_ARRAY_LEN(sizes)]; memcpy(bytes + 8 * 60 + 0x18, &v, 4);
        v = versions[(pick >> 24) % RT_ARRAY_LEN(versions)]; memcpy(bytes + 8 * 60 + 0x1c, &v, 4);
        static const uint32_t elements[] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 0xffffffffu, 0x80000000u};
        static const uint32_t calc_types[] = {0, 1, 2, 2, 2, 3, 0xffffffffu};
        static const uint32_t skill_ids[] = {1, 0, 2, 0x9d, 0x9e, 0x9f, 0xa0, 0x2b9, 0x2ba, 0x2bb, 0x2c3, 0x2c4, 0x2c9, 0x2ca,
                                             0x2d9, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0x28, 0x100};
        v = elements[(pick >> 28) % RT_ARRAY_LEN(elements)]; memcpy(bytes + 8 * 70 + 0x38, &v, 4);
        v = calc_types[(pick >> 32) % RT_ARRAY_LEN(calc_types)]; memcpy(bytes + 8 * 70 + 0x24, &v, 4);
        r = random_word(state);
        v = (r & 3) == 0 ? (uint32_t)(r >> 8) % 0x300 : skill_ids[(r >> 8) % RT_ARRAY_LEN(skill_ids)]; memcpy(bytes + 8 * 70 + 0x14, &v, 4);
        region[80 + 0xe8 / 8] = WORD(112);                     /* GlobalWork stand-in +0xe8 _belomu_barrior_list */
        static const uint32_t lengths[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 0xffffffffu};
        v = lengths[(pick >> 36) % RT_ARRAY_LEN(lengths)]; memcpy(bytes + 8 * 112 + 0x18, &v, 4);
        static const unsigned char entries[] = {0, 0, 1, 1, 2, 0xff};
        for (unsigned i = 0; i < 16; ++i) bytes[8 * 112 + 0x20 + i] = entries[random_word(state) % RT_ARRAY_LEN(entries)];
        v = ((pick >> 40) & 7) == 0 ? 0 : (uint32_t)(r = random_word(state)) | 1; memcpy(bytes + 8 * 120 + 0xe0, &v, 4);   /* class initialized word */
        region[150] = WORD(120);                               /* the slot's value points at a word that holds the class */
        region[152] = WORD(154);                               /* LIST_CONTEXT chain: [[slot]] = 154, +0x20 -> 160, +0xc0 -> 190, +0xc8 any */
        region[154 + 4] = WORD(160);
        region[160 + 0xc0 / 8] = WORD(190);
        *flag = ((pick >> 44) & 15) == 0 ? 0 : 1;               /* lazy initialization pending one time in sixteen */
        *slot_class = WORD(150);
        *slot_context = WORD(152);
        *slot_add = WORD(230);
        tl_stub_instance = WORD(80);
        #undef WORD
        c->x[0] = rb;                                          /* the executer */
        c->x[1] = rb + 8 * 70;                                 /* the skill object */
        c->x[2] = rb + 8 * 240;                                /* the ratio cell */
        break;
    }
    case 0x71021ff580ULL: { /* Battle.SkillExecter.damage_calc (task-60): the executer (+0x10 monitor, +0x20 acter, +0x28 target, +0x30 skill object,
                             * +0x40 _tmp_value), the target (+0x20 id, +0x76 _is_guard, +0x8c counter, +0x98/+0xa0 managers, +0xa8 element manager with its int[],
                             * +0xb8 result logger with +0x50 List<int>), the acter, the skill object, the BattleManager class cell, the System.Math class cell,
                             * the byte flag, the GlobalWork stand-in with its bool[], and every flag byte and metadata slot of this row and its nested ports
                             * pointed at private cells and restored after the row. */
        static uint64_t region[1000];
        if (!driver_have_saved) {
            for (int i = 0; i < 4; ++i) driver_saved[DRIVER_SLOTS + i] = *(volatile uint8_t *)(uintptr_t)driver_flag_address[i];
            for (int i = 0; i < DRIVER_SLOTS; ++i) driver_saved[i] = *(uint64_t *)(uintptr_t)driver_slot_at(i);
            driver_have_saved = 1;
        }
        fill_fake_region(region, 1000, state);
        unsigned char *bytes = (unsigned char *)region;
        const uint64_t rb = (uint64_t)(uintptr_t)region;
        uint64_t r;
        #define WORD(n) (rb + 8 * (n))
        #define PUT32(word, off, value) do { uint32_t put_ = (value); memcpy(bytes + 8 * (word) + (off), &put_, 4); } while (0)
        region[2] = WORD(600);
        region[3] = WORD(650);                                  /* +0x18 _cur_skill_result */
        region[4] = WORD(140);
        region[5] = WORD(24);
        region[6] = WORD(70);
        static const uint32_t tmp_values[] = {0, 1, 2, 3, 7, 100, 9999, 0xffffffffu, 0xfffffff9u, 0x7fffffffu, 0x80000000u, 0x80000001u, 0x3f800000u,
                                              0x7f800000u, 0x4b800000u, 0x20000000u, 0x55555555u};
        r = random_word(state);
        PUT32(0, 0x40, (r & 7) == 0 ? (uint32_t)(r >> 8) : tmp_values[(r >> 8) % RT_ARRAY_LEN(tmp_values)]);
        static const uint32_t skill_ids[] = {1, 0x2c3, 0x2c3, 0x2bd, 0x2bd, 0x2c4, 0x2c5, 0x2e7, 0x2e7, 103, 104, 105, 106, 107, 108, 102, 999, 1000, 1001,
                                             0x7fffffffu, 0x80000000u, 0xffffffffu, 0x28, 0x9e, 0x9f, 0x2ba, 0x2c9, 0x100, 0x2c2, 0x2c6};
        static const uint32_t calc_types[] = {0, 1, 2, 4, 4, 8, 8, 9, 9, 10, 10, 3, 0xffffffffu};
        static const uint32_t skill_types[] = {13, 13, 13, 0, 1, 2, 12, 14, 0xffffffffu, 0x80000000u};
        static const uint32_t elements[] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 0xffffffffu, 0x80000000u};
        r = random_word(state);
        PUT32(70, 0x14, (r & 7) == 0 ? (uint32_t)(r >> 8) % 0x400 : skill_ids[(r >> 8) % RT_ARRAY_LEN(skill_ids)]);
        PUT32(70, 0x24, calc_types[(pick >> 8) % RT_ARRAY_LEN(calc_types)]);
        PUT32(70, 0x20, skill_types[(pick >> 12) % RT_ARRAY_LEN(skill_types)]);
        PUT32(70, 0x38, elements[(pick >> 16) % RT_ARRAY_LEN(elements)]);
        static const uint32_t ids[] = {0x649, 0x649, 0x64f, 0x64f, 0x64f, 0x64f, 0, 1, 0x648, 0x650, 0xffffffffu, 0x2c3};
        static const uint32_t unported[] = {0x5dd, 0x5ec, 0x615, 0x653};   /* task-61: the four enemy rules, ported */
        r = random_word(state);
        PUT32(24, 0x20, (r & 7) == 0 ? unported[(r >> 8) & 3] : ids[(r >> 12) % RT_ARRAY_LEN(ids)]);
        static const unsigned char guards[] = {0, 0, 1, 2, 0x80, 0xff};
        bytes[8 * 24 + 0x76] = guards[(pick >> 20) % RT_ARRAY_LEN(guards)];
        static const uint32_t counters[] = {0, 1, 1, 2, 3, 4, 6, 0xffffffffu, 0x80000000u};
        PUT32(24, 0x8c, counters[(pick >> 24) % RT_ARRAY_LEN(counters)]);
        region[24 + 0xa8 / 8] = WORD(80);
        region[24 + 0xb8 / 8] = WORD(200);
        region[80 + 2] = WORD(84);
        static const uint32_t lengths[] = {0, 1, 2, 3, 4, 5, 6, 9};
        PUT32(84, 0x18, lengths[(pick >> 28) % RT_ARRAY_LEN(lengths)]);
        for (unsigned i = 0; i < 12; ++i) {
            uint64_t q = random_word(state);
            PUT32(84, 0x20 + 4 * i, (q & 7) == 0 ? (uint32_t)(q >> 8) : (uint32_t)((q >> 8) % 7));
        }
        static const unsigned char shoes[] = {0, 0, 1, 2, 0xff};
        bytes[8 * 140 + 0x7d] = shoes[(pick >> 32) % RT_ARRAY_LEN(shoes)];
        static const uint32_t statuses[] = {5, 5, 5, 0, 1, 4, 6, 0xffffffffu};
        static const unsigned char buff_flags[] = {0, 0, 1, 1, 2, 0xff};
        static const unsigned character_word[3] = {24, 140, 170};
        static const unsigned manager_word[3] = {520, 540, 560};
        for (unsigned k = 0; k < 3; ++k) {
            region[character_word[k] + 0x98 / 8] = WORD(manager_word[k]);
            region[character_word[k] + 0xa0 / 8] = WORD(manager_word[k] + 8);
            PUT32(manager_word[k], 0x10, statuses[random_word(state) % RT_ARRAY_LEN(statuses)]);
            bytes[8 * (manager_word[k] + 8) + 0x10] = buff_flags[random_word(state) % RT_ARRAY_LEN(buff_flags)];
            bytes[8 * (manager_word[k] + 8) + 0x11] = buff_flags[random_word(state) % RT_ARRAY_LEN(buff_flags)];
            PUT32(manager_word[k] + 8, 0x28, battle_edge_word(state));
            PUT32(manager_word[k] + 8, 0x2c, battle_edge_word(state));
        }
        region[580 + 4] = WORD(170);
        region[200 + 0x50 / 8] = ((pick >> 36) & 7) == 0 ? 0 : WORD(220);
        static const uint32_t sizes[] = {0, 0, 1, 2, 3, 5, 0xffffffffu, 0x80000000u};
        static const uint32_t versions[] = {0, 1, 0xffffffffu, 0x7fffffffu, 5};
        region[220 + 2] = WORD(226);
        PUT32(220, 0x18, sizes[(pick >> 40) % RT_ARRAY_LEN(sizes)]);
        PUT32(220, 0x1c, versions[(pick >> 44) % RT_ARRAY_LEN(versions)]);
        region[230 + 2] = WORD(234);
        PUT32(230, 0x18, 0);
        region[96 + 0xe8 / 8] = WORD(130);
        static const uint32_t blengths[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 0xffffffffu};
        PUT32(130, 0x18, blengths[(pick >> 48) % RT_ARRAY_LEN(blengths)]);
        static const unsigned char entries[] = {0, 0, 1, 1, 2, 0xff};
        for (unsigned i = 0; i < 16; ++i) bytes[8 * 130 + 0x20 + i] = entries[random_word(state) % RT_ARRAY_LEN(entries)];
        region[240] = WORD(244);
        region[244 + 0xb8 / 8] = WORD(280);
        region[280 + 2] = WORD(290);
        region[290 + 0xd8 / 8] = WORD(345);
        region[290 + 0x60 / 8] = WORD(310);                     /* the instance's +0x60 object (the lifted-only character 0x615 path) */
        region[345 + 2] = WORD(350);
        static const uint32_t encounters[] = {0x819, 0x819, 0, 1, 0x818, 0x81a, 0xffffffffu, 0x80000000u};
        PUT32(290, 0x168, encounters[(pick >> 52) % RT_ARRAY_LEN(encounters)]);
        static const uint32_t manager_counts[] = {0, 1, 6, 7, 7, 8, 100, 0xffffffffu, 0x80000000u, 0x7fffffffu};
        PUT32(290, 0xd0, manager_counts[(random_word(state) >> 8) % RT_ARRAY_LEN(manager_counts)]);   /* task-61: the 0x5dd rule's threshold (7) */
        static const unsigned char object_flags[] = {0, 1, 1, 2, 0xff};
        bytes[8 * 310 + 0x20] = object_flags[(random_word(state) >> 8) % RT_ARRAY_LEN(object_flags)];   /* task-61: the 0x615 rule's flag byte */
        bytes[8 * 200 + 0x4a] = (random_word(state) & 3) == 0 ? 1 : 0;                                      /* task-61: the logger flag the rules set */
        bytes[8 * 96 + 0xd8] = (random_word(state) & 3) == 0 ? 1 : 0;                                       /* task-61: the GlobalWork instance flag */
        r = random_word(state);
        PUT32(244, 0xe0, (r & 3) == 0 ? 0 : (uint32_t)(r >> 8) | 1);
        region[360] = WORD(364);
        r = random_word(state);
        PUT32(364, 0xe0, (r & 3) == 0 ? 0 : (uint32_t)(r >> 8) | 1);
        region[410] = WORD(414);
        region[414 + 0xb8 / 8] = WORD(438);                      /* task-61: GlobalWork's static fields; the first one is the instance the stand-in get_inst returns */
        region[438] = WORD(96);
        r = random_word(state);
        PUT32(414, 0xe0, (r & 7) == 0 ? 0 : (uint32_t)(r >> 8) | 1);
        bytes[8 * 400] = ((pick >> 56) & 3) == 0 ? 0 : (((pick >> 58) & 7) == 0 ? 0x80 : 1);
        for (int i = 0; i < DRIVER_SLOTS; ++i) {
            region[800 + 2 * i] = WORD(801 + 2 * i);
            *(uint64_t *)(uintptr_t)driver_slot_at(i) = WORD(800 + 2 * i);
        }
        *(uint64_t *)(uintptr_t)0x7103a57e48ULL = WORD(240);
        *(uint64_t *)(uintptr_t)0x7103a581d0ULL = WORD(400);
        *(uint64_t *)(uintptr_t)0x7103a46a90ULL = WORD(360);
        *(uint64_t *)(uintptr_t)0x7103a58048ULL = WORD(410);
        region[450] = WORD(452);
        region[452 + 4] = WORD(460);
        region[460 + 0xc0 / 8] = WORD(470);
        *(uint64_t *)(uintptr_t)0x7103a4e8c8ULL = WORD(450);
        r = random_word(state);
        *(volatile uint8_t *)(uintptr_t)driver_flag_address[0] = ((r >> 8) & 15) == 0 ? 0 : 1;
        for (int i = 1; i < 4; ++i) *(volatile uint8_t *)(uintptr_t)driver_flag_address[i] = ((random_word(state) >> 8) & 31) == 0 ? 0 : 1;
        tl_stub_instance = WORD(96);
        tl_stub_new = WORD(230);
        tl_stub_current = WORD(580);
        #undef PUT32
        #undef WORD
        c->x[0] = rb;
        break;
    }
    case 0x71021f5ed0ULL: { /* Battle.SkillExecter.calc_renkei_ratio (task-58): the executer (+0x10 monitor, +0x28 target, +0x30 skill object), the target
                             * (+0x8c hit counter, +0xb8 result logger with +0x2c state and +0x50 list), the skill object (+0x14 id), the damage cell,
                             * the stand-in List<int> that Object.new returns, a class cell with its initialized word (+0xe0), the flag byte and
                             * the 13 metadata slots, pointed at private cells and restored after the row. */
        static uint64_t region[256];
        volatile uint8_t *flag = (volatile uint8_t *)(uintptr_t)(0x7103cba000ULL + 0xd5e);
        if (!renkei_have_saved) {
            renkei_saved[0] = *flag;
            for (int i = 0; i < 13; ++i) renkei_saved[1 + i] = *(uint64_t *)(uintptr_t)renkei_slot_address[i];
            renkei_have_saved = 1;
        }
        fill_fake_region(region, 256, state);
        unsigned char *bytes = (unsigned char *)region;
        const uint64_t rb = (uint64_t)(uintptr_t)region;
        uint64_t r;
        uint32_t v;
        #define WORD(n) (rb + 8 * (n))
        region[2] = WORD(220);                                  /* executer +0x10 _monitor */
        region[5] = WORD(24);                                   /* +0x28 _target */
        region[6] = WORD(70);                                   /* +0x30 _skill_obj */
        static const uint32_t counters[] = {0, 1, 1, 2, 3, 4, 6, 0xffffffffu, 0x80000000u};
        v = counters[(pick >> 8) % RT_ARRAY_LEN(counters)]; memcpy(bytes + 8 * 24 + 0x8c, &v, 4);   /* target _target_counter */
        region[24 + 0xb8 / 8] = WORD(48);                       /* target +0xb8 _result_logger */
        region[48 + 0x50 / 8] = WORD(60);                       /* logger +0x50 _multi_number (the row replaces it) */
        region[100 + 2] = WORD(104);                            /* the stand-in List<int>: +0x10 _items */
        memcpy(bytes + 8 * 100 + 0x18, &(uint32_t){0}, 4);
        static const uint32_t skill_ids[] = {0x2ba, 0x2bd, 0x2c3, 0x2c3, 0x2c3, 0x2c9, 0x2d2, 0x2b9, 0x2bb, 0x2bc, 0x2be, 0x2c2, 0x2c4, 0x2c8, 0x2ca,
                                             0x2d1, 0x2d3, 0, 1, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0x100};
        r = random_word(state);
        v = (r & 7) == 0 ? (uint32_t)(r >> 8) % 0x300 : skill_ids[(r >> 8) % RT_ARRAY_LEN(skill_ids)]; memcpy(bytes + 8 * 70 + 0x14, &v, 4);
        static const uint32_t damages[] = {0, 1, 7, 8, 9, 100, 9999, 0xffffffffu, 0xfffffff9u, 0xfffffff8u, 0x7fffffffu, 0x80000000u, 0x20000000u,
                                           0x40000000u, 0x1fffffffu, 0x10000000u, 0x3f800000u, 0x7f800000u, 0x4b800000u, 0x3b000000u};
        r = random_word(state);
        v = (r & 7) == 0 ? (uint32_t)(r >> 8) : damages[(r >> 8) % RT_ARRAY_LEN(damages)]; memcpy(bytes + 8 * 200, &v, 4);   /* the damage cell */
        v = ((pick >> 20) & 3) == 0 ? 0 : (uint32_t)(r = random_word(state)) | 1; memcpy(bytes + 8 * 122 + 0xe0, &v, 4);   /* class initialized word */
        region[120] = WORD(122);                                /* the class cell holds the class pointer */
        for (unsigned i = 0; i < 13; ++i) {                     /* every metadata slot points at a cell whose first word is another pointer */
            region[160 + i] = WORD(180 + i);
            *(uint64_t *)(uintptr_t)renkei_slot_address[i] = WORD(160 + i);
        }
        *(uint64_t *)(uintptr_t)renkei_slot_address[3] = WORD(120);   /* the class slot */
        *flag = ((pick >> 24) & 15) == 0 ? 0 : 1;               /* lazy initialization pending one time in sixteen */
        tl_stub_new = WORD(100);
        #undef WORD
        c->x[0] = rb;                                           /* the executer */
        c->x[1] = rb + 8 * 200;                                 /* the damage cell */
        break;
    }
    case 0x7102201aa0ULL: { /* Battle.SkillExecter.group_a_b (task-59): the executer (+0x10 monitor, +0x20 acter, +0x28 target, +0x30 skill object), the target
                             * (+0x76 _is_guard), the skill object (+0x20 _skill_type, 13 = RENKEI), the damage cell, the BattleManager class cell (class
                             * +0xb8 statics, +0xe0 initialized word; statics +0x10 instance; instance +0xd8 _pc_list; +0x10 its list), the byte flag cell,
                             * the stand-in enumerator element, the flag byte and nine metadata slots, pointed at private cells and restored after the row. */
        static uint64_t region[320];
        volatile uint8_t *flag = (volatile uint8_t *)(uintptr_t)(0x7103cba000ULL + 0xd73);
        if (!group_have_saved) {
            group_saved[0] = *flag;
            for (int i = 0; i < 9; ++i) group_saved[1 + i] = *(uint64_t *)(uintptr_t)group_slot_address[i];
            group_have_saved = 1;
        }
        fill_fake_region(region, 320, state);
        unsigned char *bytes = (unsigned char *)region;
        const uint64_t rb = (uint64_t)(uintptr_t)region;
        uint64_t r;
        uint32_t v;
        #define WORD(n) (rb + 8 * (n))
        region[2] = WORD(220);                                  /* executer +0x10 _monitor */
        region[4] = WORD(8);                                    /* +0x20 _acter */
        region[5] = WORD(24);                                   /* +0x28 _target */
        region[6] = WORD(70);                                   /* +0x30 _skill_obj */
        static const unsigned char guards[] = {0, 0, 1, 2, 0x80, 0xff};
        bytes[8 * 24 + 0x76] = guards[(pick >> 8) % RT_ARRAY_LEN(guards)];   /* target _is_guard */
        static const uint32_t types[] = {13, 13, 13, 13, 13, 13, 0, 1, 2, 12, 14, 0xffffffffu, 0x80000000u, 0xc, 0xe};
        v = types[(pick >> 12) % RT_ARRAY_LEN(types)]; memcpy(bytes + 8 * 70 + 0x20, &v, 4);   /* skill object _skill_type */
        static const uint32_t damages[] = {0, 1, 2, 3, 7, 100, 9999, 0xffffffffu, 0xfffffffeu, 0xfffffff9u, 0x7fffffffu, 0x80000000u, 0x80000001u,
                                           0x55555555u, 0x2aaaaaaau, 0x3f800000u, 0x7f800000u, 0x4b800000u, 0x3b000000u};
        r = random_word(state);
        v = (r & 7) == 0 ? (uint32_t)(r >> 8) : damages[(r >> 8) % RT_ARRAY_LEN(damages)]; memcpy(bytes + 8 * 90, &v, 4);   /* the damage cell */
        region[100] = WORD(104);                                /* the class cell holds the class pointer */
        region[104 + 0xb8 / 8] = WORD(140);                     /* class +0xb8 static fields */
        region[140 + 2] = WORD(150);                            /* statics +0x10 _instance */
        region[150 + 0xd8 / 8] = WORD(190);                     /* instance +0xd8 _pc_list */
        region[190 + 2] = WORD(200);                            /* _pc_list +0x10 the list */
        v = ((pick >> 20) & 3) == 0 ? 0 : (uint32_t)(r = random_word(state)) | 1; memcpy(bytes + 8 * 104 + 0xe0, &v, 4);   /* class initialized word */
        bytes[8 * 230] = ((pick >> 24) & 3) == 0 ? 0 : (((pick >> 26) & 7) == 0 ? 0x80 : 1);   /* the static-field byte flag */
        for (unsigned i = 0; i < 9; ++i) *(uint64_t *)(uintptr_t)group_slot_address[i] = WORD(232 + 2 * i);   /* each slot points at a cell */
        *(uint64_t *)(uintptr_t)group_slot_address[0] = WORD(100);   /* the BattleManager class slot */
        *(uint64_t *)(uintptr_t)group_slot_address[1] = WORD(230);   /* the pointer to the byte flag */
        /* the characters' buff and bad status managers: is_attack_up +0x10, is_defense_up +0x11 (bytes at +0xa0), is_fear: status == 5 (+0x98 -> +0x10) */
        static const uint32_t statuses[] = {5, 5, 5, 0, 1, 4, 6, 0xffffffffu};
        static const unsigned char buff_flags[] = {0, 0, 1, 1, 2, 0xff};
        static const unsigned character_word[3] = {8, 24, 160};   /* acter, target, element */
        for (unsigned k = 0; k < 3; ++k) {
            region[character_word[k] + 0x98 / 8] = WORD(260 + 8 * k);
            region[character_word[k] + 0xa0 / 8] = WORD(264 + 8 * k);
            v = statuses[random_word(state) % RT_ARRAY_LEN(statuses)]; memcpy(bytes + 8 * (262 + 8 * k), &v, 4);
            bytes[8 * (266 + 8 * k)] = buff_flags[random_word(state) % RT_ARRAY_LEN(buff_flags)];
            bytes[8 * (266 + 8 * k) + 1] = buff_flags[random_word(state) % RT_ARRAY_LEN(buff_flags)];
        }
        region[224] = WORD(160);                                /* element +0x20: a character */
        *flag = ((pick >> 32) & 15) == 0 ? 0 : 1;               /* lazy initialization pending one time in sixteen */
        tl_stub_current = WORD(220);
        #undef WORD
        c->x[0] = rb;                                           /* the executer */
        c->x[1] = rb + 8 * 90;                                  /* the damage cell */
        break;
    }
    case 0x71021f7ea0ULL: { /* Battle.SkillExecter.set_acction_command_ratio (task-56): +0x18 result record, +0x20 acter param (+0x6c _is_player),
                             * +0x30 skill object (+0x14 _skill_id). Every skill id of the 129 entry table and its neighbors, enemy and player. */
        fill_fake_region(fake, 64, state);
        unsigned char *bytes = (unsigned char *)fake;
        fake[3] = base + 8 * 24;
        fake[4] = base + 8 * 8;
        fake[6] = base + 8 * 40;
        static const unsigned char flags[] = {0, 0, 1, 1, 1, 2, 0x80, 0xff};
        bytes[8 * 8 + 0x6c] = flags[(pick >> 8) % RT_ARRAY_LEN(flags)];
        static const uint32_t edge_ids[] = {0x2d6, 0x2d7, 0x2d8, 0, 1, 2, 0xffffffffu, 0x80000000u, 0x7fffffffu, 0x80, 0x81, 0x82, 0x83, 0x100};
        uint32_t id = ((pick >> 20) & 7) == 0 ? edge_ids[(pick >> 24) % RT_ARRAY_LEN(edge_ids)] : (uint32_t)((pick >> 24) % 0x90);
        memcpy(bytes + 8 * 40 + 0x14, &id, 4);
        c->x[0] = base;
        break;
    }
    case 0x71021be280ULL: { /* Battle.BattleCharacterParam.use_damage_guard (task-56): param +0xb8 -> logger, flag byte at +0x49 */
        fill_fake_region(fake, 64, state);
        unsigned char *bytes = (unsigned char *)fake;
        fake[0xb8 / 8] = base + 8 * 32;
        static const unsigned char flag_values[] = {0, 0, 1, 2, 0xff};
        bytes[8 * 32 + 0x49] = flag_values[(pick >> 8) % RT_ARRAY_LEN(flag_values)];
        c->x[0] = base;
        break;
    }
    case 0x71021be3c0ULL: { /* Battle.BattleCharacterParam.calc_element (task-55): param +0xa8 -> ElementManager, +0x10 -> int[] (length +0x18, elements +0x20) */
        fill_fake_region(fake, 64, state);
        unsigned char *bytes = (unsigned char *)fake;
        fake[0xa8 / 8] = base + 8 * 24;
        fake[24 + 2] = base + 8 * 32;
        static const uint32_t lengths[] = {0, 1, 2, 3, 4, 5, 6, 9, 0xffffffffu};
        uint32_t length = lengths[(pick >> 8) % RT_ARRAY_LEN(lengths)];
        memcpy(bytes + 8 * 32 + 0x18, &length, 4);
        for (unsigned i = 0; i < 12; ++i) {   /* element states: 0..6 mostly (1 to 4 select a table entry), a few wild ones */
            uint64_t r = random_word(state);
            uint32_t state_value = (r & 7) == 0 ? (uint32_t)(r >> 8) : (uint32_t)((r >> 8) % 7);
            memcpy(bytes + 8 * 32 + 0x20 + 4 * i, &state_value, 4);
        }
        c->x[0] = base;
        c->x[1] = ((pick >> 16) & 7) == 0 ? (random_word(state) & ~0xffffffffULL) | (uint32_t)(int32_t)((int)((pick >> 20) % 12) - 3)
                                          : (random_word(state) & ~0xffffffffULL) | ((pick >> 24) % 8);
        break;
    }
    case 0x71021bc270ULL:   /* Battle.BattleCharacterParam get_calc_defence and get_calc_magic_defence (task-54), same fake graph: */
    case 0x71021bc440ULL: { /* param +0xa0 -> buff manager, buff +0x18 -> int[] (length +0x18, elements +0x20), stats +0x40, +0x48, _is_player +0x6c */
        fill_fake_region(fake, 64, state);
        static const uint32_t edge_ints[] = {0, 1, 0xffffffffu, 100, 0xffffff9cu, 0x00ffffffu, 0x7fffffffu, 0x80000000u, 0x01234567u};
        unsigned char *bytes = (unsigned char *)fake;
        const unsigned buff_word = 24, array_word = 32;
        fake[0xa0 / 8] = base + 8 * buff_word;
        fake[buff_word + 3] = base + 8 * array_word;
        static const uint32_t lengths[] = {0, 1, 2, 3, 4, 5, 9, 0xffffffffu};
        uint32_t length = lengths[(pick >> 8) % RT_ARRAY_LEN(lengths)];
        memcpy(bytes + 8 * array_word + 0x18, &length, 4);
        for (unsigned i = 0; i < 6; ++i) {
            uint64_t r = random_word(state);
            uint32_t v = (r & 3) == 0 ? (uint32_t)(r >> 8) : edge_ints[(r >> 8) % RT_ARRAY_LEN(edge_ints)];
            unsigned at = i < 4 ? 8 * array_word + 0x20 + 4 * i : i == 4 ? 0x40 : 0x48;
            memcpy(bytes + at, &v, 4);
        }
        static const unsigned char flags[] = {0, 0, 1, 2, 0x80, 0xff};
        bytes[0x6c] = flags[(pick >> 16) % RT_ARRAY_LEN(flags)];
        c->x[0] = base;
        static const uint32_t fpcr_modes[] = {0, 0, 0, 1u << 24, 1u << 25, (1u << 24) | (1u << 25), 1u << 22, 2u << 22, 3u << 22};
        c->fpcr = fpcr_modes[(pick >> 24) % RT_ARRAY_LEN(fpcr_modes)];
        c->fpsr = (uint32_t)(pick >> 32) & 0x0800009fu;
        break;
    }
    default:
        break;
    }
}

static void selftest(Leaf *leaf, GuestFn entry) {
    if (leaf->probe) return;   /* traced rows have no Rust body to compare */
    /* A caller's lifted body goes through the other leaves' wrappers, which count calls and differences.
     * Keep those counters as they were so only real game calls reach the live summary; any difference a
     * callee finds during this selftest is a failure. */
    static unsigned long long saved_calls[leaf_count], saved_differences[leaf_count];   /* static: thousands of rows */
    const int is_caller = leaf->rust_regs != 0;
    const int stores = is_store(leaf);   /* only a caller's lifted body reaches other leaves' wrappers */
    if (is_caller)
        for (int i = 0; i < leaf_count; ++i) {
            saved_calls[i] = atomic_load(&leaves[i].calls);
            saved_differences[i] = atomic_load(&leaves[i].differences);
        }
    uint64_t seed = leaf->address;
    if (leaf->replay) tl_stub = (seed * 0x9e3779b97f4a7c15ULL) | 1;   /* traced callees are stand-ins during the selftest */
    uint64_t vseed = leaf->address ^ 0x9e3779b97f4a7c15ULL;   /* separate stream: vector registers, so the integer cases stay as before */
    uint64_t fake[64];
    unsigned long long stores_seen = 0;   /* runs where the footprint changed, so a no-op pair cannot pass */
    const int pointer_leaf = reads_x0_pointer(leaf->address);
    /* 64-bit boundaries plus the byte, short and 20-bit limits that clamp-style leaves care about. */
    const uint64_t edges[] = {0, 1, 2, 0x1f, 0x20, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
                              0x7fffffffULL, 0x80000000ULL, 0xffffffffULL, 0x100000000ULL,
                              UINT64_MAX, UINT64_MAX - 0x7f, UINT64_MAX - 0x80, UINT64_MAX - 0x7fff,
                              UINT64_MAX - 0x8000, 0xffffffff80000000ULL, INT64_MAX, (uint64_t)INT64_MIN};
    const unsigned nedge = RT_ARRAY_LEN(edges);
    const unsigned edge_cases = nedge * nedge * nedge;
    for (unsigned i = 0; i < 10000 + edge_cases; ++i) {
        Ctx c;
        memset(&c, 0xa5, sizeof c);
        for (unsigned j = 0; j < 32; ++j) c.x[j] = random_word(&seed);
        for (unsigned j = 0; j < 32; ++j) {   /* task-50: random vector registers (a constant fill hides lane bugs) */
            v128 r = {(long long)random_word(&vseed), (long long)random_word(&vseed)};
            c.v[j] = r;
        }
        c.exc = 0;   /* a real call never starts with a pending exception; lifted callers check it after each call */
        if (i < edge_cases) {
            unsigned k = i;
            for (unsigned j = 0; j < 3; ++j, k /= nedge) c.x[j] = edges[k % nedge];
        }
        if (leaf->rust_regs) prepare_caller(leaf->address, &c, fake, &seed, i < edge_cases);
        if (battle_object_kind(leaf->address)) prepare_battle(battle_object_kind(leaf->address), &c, fake, &seed);
        if (pointer_leaf) {
            fill_fake_region(fake, RT_ARRAY_LEN(fake), &seed);
            /* null one time in eight, otherwise an 8-byte aligned slot with room for the +0x10 field */
            uint64_t pick = random_word(&seed);
            c.x[0] = (pick & 7) == 0 ? 0 : (uint64_t)(uintptr_t)&fake[(pick >> 8) % (RT_ARRAY_LEN(fake) - 3)];
        }
        if (leaf->load_kind) {
            /* generated getters (task-45): x[base] + offset is an 8-byte aligned slot inside the private fake
             * region; the null-safe form also gets a null base one time in eight */
            fill_fake_region(fake, RT_ARRAY_LEN(fake), &seed);
            uint64_t pick = random_word(&seed);
            uint64_t at = (uint64_t)(uintptr_t)&fake[(pick >> 8) % 56];
            c.x[leaf->base_reg] = (leaf->load_kind == 2 && (pick & 7) == 0) ? 0 : at - leaf->offset;
            if (leaf->load_kind == 3) {
                /* through a field: the slot holds a pointer whose target (+ offset2) is another slot of the region */
                uint64_t target = (uint64_t)(uintptr_t)&fake[(pick >> 20) % 56];
                *(uint64_t *)(uintptr_t)at = target - leaf->offset2;
            }
            /* boolean getters compare the field with x1 or x2: make them equal a quarter of the time, and equal
             * in the low 32 bits only one time in eight */
            uint64_t value = 0;
            memcpy(&value, (const void *)(uintptr_t)at, leaf->bytes < 8 ? leaf->bytes : 8);
            unsigned mode_pick = (unsigned)(pick >> 40) & 7;
            if (leaf->base_reg != 1 && mode_pick < 2) c.x[1] = value;
            if (leaf->base_reg != 2 && mode_pick < 2) c.x[2] = value;
            if (leaf->base_reg != 1 && mode_pick == 2) c.x[1] = value ^ (random_word(&seed) << 32);
            if (leaf->base_reg != 2 && mode_pick == 2) c.x[2] = value ^ (random_word(&seed) << 32);
        }
        if (stores) {
            /* store leaves write only into the private fake region, never game memory; x[base] + offset is
             * an 8-byte aligned slot with the whole footprint inside the region */
            fill_fake_region(fake, RT_ARRAY_LEN(fake), &seed);
            c.x[leaf->base_reg] = (uint64_t)(uintptr_t)&fake[(random_word(&seed) >> 8) % 40] - leaf->offset;
            unsigned char before[MAX_FOOTPRINT];
            const unsigned char *at = (const unsigned char *)(uintptr_t)(c.x[leaf->base_reg] + leaf->offset);
            memcpy(before, at, leaf->bytes);
            entry(&c);
            stores_seen += memcmp(before, at, leaf->bytes) != 0;
            continue;
        }
        entry(&c);
    }
    for (int i = 0; is_caller && i < leaf_count; ++i) {
        if (&leaves[i] == leaf) continue;
        if (atomic_load(&leaves[i].differences) != saved_differences[i]) {
            rt_log("decomp selftest: callee %#llx differs while testing %#llx (%llu new differences)", (unsigned long long)leaves[i].address,
                   (unsigned long long)leaf->address, (unsigned long long)(atomic_load(&leaves[i].differences) - saved_differences[i]));
            rt_exit(10, "decomp selftest: callee differs");
        }
        atomic_store(&leaves[i].calls, saved_calls[i]);
    }
    if (stores && !stores_seen) rt_exit(10, "decomp selftest: store leaf never wrote");
    tl_stub = 0;
    restore_attack_globals();
    restore_belome_globals();
    restore_renkei_globals();
    restore_group_globals();
    restore_driver_globals();
    atomic_store(&leaf->deferred, 0);
    unsigned long long calls = atomic_exchange(&leaf->calls, 0);
    unsigned long long differences = atomic_exchange(&leaf->differences, 0);
    if (atomic_load(&leaf->raced)) rt_exit(10, "decomp selftest: raced on a private region");
    rt_log("decomp selftest addr=%#llx random=10000 edge=%u calls=%llu differences=%llu",
           (unsigned long long)leaf->address, edge_cases, calls, differences);
    if (calls != 10000 + edge_cases || differences) rt_exit(10, "decomp selftest failed");
}

void decomp_start(void) {
    const char *value = getenv("SMRPG_DECOMP");
    if (!value || !*value) value = "on";   /* default on (owner accepted task-29); off and ab stay selectable */
    if (!strcmp(value, "off")) return;
    if (!strcmp(value, "ab")) mode = AB;
    else if (!strcmp(value, "on")) mode = ON;
    else {
        rt_log("unknown SMRPG_DECOMP value; using on");
        return;
    }
    const char *test = getenv("SMRPG_DECOMP_SELFTEST");
    if (mode == AB) decomp_fp_shadow_enable();
    if (mode == AB && test && !strcmp(test, "1")) decomp_fp_selftest();
    for (int i = 0; i < leaf_count; ++i) {
        Leaf *leaf = &leaves[i];
        GuestFn entry = rt_lookup(leaf->address);
        if (entry != leaf->entry) rt_exit(10, "decomp registry mismatch");
        if (mode == AB && test && !strcmp(test, "1")) selftest(leaf, entry);
    }
    rt_log("decomp mode=%s registered=%d", value, leaf_count);
    rule_count_live = 1;
}

void decomp_summary(void) {
    if (mode != OFF)
        rt_log("decomp damage_calc by target: 0x5dd=%llu 0x5ec=%llu 0x615=%llu 0x653=%llu other=%llu", atomic_load(&rule_calls[0]),
               atomic_load(&rule_calls[1]), atomic_load(&rule_calls[2]), atomic_load(&rule_calls[3]), atomic_load(&rule_calls[4]));
    if (mode == ON) {
        for (int i = 0; i < leaf_count; ++i)
            if (leaves[i].rust_regs) rt_log("decomp on addr=%#llx calls=%llu", (unsigned long long)leaves[i].address, atomic_load(&leaves[i].calls));
        return;
    }
    if (mode != AB) return;
    decomp_fp_shadow_summary();
    for (int i = 0; i < leaf_count; ++i) {
        Leaf *leaf = &leaves[i];
        rt_log("decomp live addr=%#llx calls=%llu differences=%llu", (unsigned long long)leaf->address,
               atomic_load(&leaf->calls), atomic_load(&leaf->differences));
        if (atomic_load(&leaf->deferred))
            rt_log("decomp live addr=%#llx deferred=%llu (not compared: lazy initialization pending)",
                   (unsigned long long)leaf->address, atomic_load(&leaf->deferred));
    }
    unsigned long long raced = 0;
    for (int i = 0; i < leaf_count; ++i) raced += atomic_load(&leaves[i].raced);
    rt_log("decomp raced total=%llu", raced);
}
