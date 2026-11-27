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
#include "manifest.def"
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
#include "manifest.def"
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
#include "manifest.def"
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

static void compare(Leaf *leaf, Ctx *c) {
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

static inline void dispatch(Leaf *leaf, Ctx *c) {
    if (mode == OFF) leaf->lifted(c);
    else if (mode == ON) run_rust(leaf, c);
    else compare(leaf, c);
}

#define LEAF(chunk, addr, rust) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define SLEAF(chunk, addr, rust, base, off, len) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define CLEAF(chunk, addr, rust) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define GLEAF(chunk, addr, rust, base, off, len) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define NLEAF(chunk, addr, rust, base, off, len) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define DLEAF(chunk, addr, rust, base, off, len, off2) void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#define VLEAF(chunk, addr, rust, vkind, vreg, base, off, len, off2) \
    void f_##addr(Ctx *c) { dispatch(&leaves[index_##addr], c); }
#include "manifest.def"
#undef LEAF
#undef SLEAF
#undef CLEAF
#undef GLEAF
#undef NLEAF
#undef DLEAF
#undef VLEAF

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

/* Caller functions (task-44) dereference pointers they compute, so the selftest builds a valid shape in the
 * private fake region for each one (never game memory). Register-only callers keep the random registers. */
static void prepare_caller(uint64_t address, Ctx *c, uint64_t *fake, uint64_t *state, int edge_phase) {
    uint64_t pick = random_word(state);
    uint64_t base = (uint64_t)(uintptr_t)fake;
    /* The lifted prologue pushes x29 and x30 below sp, so sp must be a real 16-byte aligned stack. The Rust
     * body has no stack; those bytes are dead once the function returns and are not compared. */
    static _Alignas(16) uint64_t stack[16];
    c->x[31] = (uint64_t)(uintptr_t)&stack[8];
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
    default:
        break;
    }
}

static void selftest(Leaf *leaf, GuestFn entry) {
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
        if (atomic_load(&leaves[i].differences) != saved_differences[i]) rt_exit(10, "decomp selftest: callee differs");
        atomic_store(&leaves[i].calls, saved_calls[i]);
    }
    if (stores && !stores_seen) rt_exit(10, "decomp selftest: store leaf never wrote");
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
    for (int i = 0; i < leaf_count; ++i) {
        Leaf *leaf = &leaves[i];
        GuestFn entry = rt_lookup(leaf->address);
        if (entry != leaf->entry) rt_exit(10, "decomp registry mismatch");
        if (mode == AB && test && !strcmp(test, "1")) selftest(leaf, entry);
    }
    rt_log("decomp mode=%s registered=%d", value, leaf_count);
}

void decomp_summary(void) {
    if (mode != AB) return;
    for (int i = 0; i < leaf_count; ++i) {
        Leaf *leaf = &leaves[i];
        rt_log("decomp live addr=%#llx calls=%llu differences=%llu", (unsigned long long)leaf->address,
               atomic_load(&leaf->calls), atomic_load(&leaf->differences));
    }
    unsigned long long raced = 0;
    for (int i = 0; i < leaf_count; ++i) raced += atomic_load(&leaves[i].raced);
    rt_log("decomp raced total=%llu", raced);
}
