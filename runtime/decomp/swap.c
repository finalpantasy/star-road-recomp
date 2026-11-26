#include "rt_internal.h"
#include "swap.h"
#include <stdatomic.h>

typedef uint64_t (*IntLeaf)(uint64_t, uint64_t, uint64_t);
/* Store leaves take a fourth argument added to every store address (0 in normal use; see compare()). */
typedef uint64_t (*StoreLeaf)(uint64_t, uint64_t, uint64_t, uint64_t);
#define MAX_FOOTPRINT 32
typedef struct {
    uint64_t address;
    GuestFn entry, lifted;
    IntLeaf rust;
    StoreLeaf rust_store;           /* set instead of rust for a leaf that writes guest memory */
    unsigned base_reg, offset, bytes; /* store footprint: bytes at x[base_reg] + offset */
    atomic_ullong calls, differences;
} Leaf;

#define LEAF(chunk, addr, rust) \
    void f_##addr(Ctx *); \
    extern void f_##addr##_lifted(Ctx *); \
    extern uint64_t rust(uint64_t, uint64_t, uint64_t);
#define SLEAF(chunk, addr, rust, base, off, len) \
    void f_##addr(Ctx *); \
    extern void f_##addr##_lifted(Ctx *); \
    extern uint64_t rust(uint64_t, uint64_t, uint64_t, uint64_t);
#include "leaves.def"
#undef LEAF
#undef SLEAF

enum {
#define LEAF(chunk, addr, rust) index_##addr,
#define SLEAF(chunk, addr, rust, base, off, len) index_##addr,
#include "leaves.def"
#undef LEAF
#undef SLEAF
    leaf_count
};

static Leaf leaves[leaf_count] = {
#define LEAF(chunk, addr, rust) {0x##addr##ULL, f_##addr, f_##addr##_lifted, rust, 0, 0, 0, 0, 0, 0},
#define SLEAF(chunk, addr, rust, base, off, len) \
    {0x##addr##ULL, f_##addr, f_##addr##_lifted, 0, rust, base, off, len, 0, 0},
#include "leaves.def"
#undef LEAF
#undef SLEAF
};

static enum { OFF, ON, AB } mode;

static void run_rust(const Leaf *leaf, Ctx *c) {
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
    trial.x[0] = leaf->rust_store(trial.x[0], trial.x[1], trial.x[2],
                                  (uint64_t)(uintptr_t)shadow - footprint);
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
    if (leaf->rust_store) {
        compare_store(leaf, c);
        return;
    }
    Ctx trial;
    memcpy(&trial, c, sizeof trial);
    run_rust(leaf, &trial);
    leaf->lifted(c);
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
#include "leaves.def"
#undef LEAF
#undef SLEAF

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

static void selftest(Leaf *leaf, GuestFn entry) {
    uint64_t seed = leaf->address;
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
        if (i < edge_cases) {
            unsigned k = i;
            for (unsigned j = 0; j < 3; ++j, k /= nedge) c.x[j] = edges[k % nedge];
        }
        if (pointer_leaf) {
            fill_fake_region(fake, RT_ARRAY_LEN(fake), &seed);
            /* null one time in eight, otherwise an 8-byte aligned slot with room for the +0x10 field */
            uint64_t pick = random_word(&seed);
            c.x[0] = (pick & 7) == 0 ? 0 : (uint64_t)(uintptr_t)&fake[(pick >> 8) % (RT_ARRAY_LEN(fake) - 3)];
        }
        if (leaf->rust_store) {
            /* store leaves write only into the private fake region, never game memory; the base register
             * is an 8-byte aligned slot with the whole footprint inside the region */
            fill_fake_region(fake, RT_ARRAY_LEN(fake), &seed);
            c.x[leaf->base_reg] = (uint64_t)(uintptr_t)&fake[(random_word(&seed) >> 8) % 40];
            unsigned char before[MAX_FOOTPRINT];
            const unsigned char *at = (const unsigned char *)(uintptr_t)(c.x[leaf->base_reg] + leaf->offset);
            memcpy(before, at, leaf->bytes);
            entry(&c);
            stores_seen += memcmp(before, at, leaf->bytes) != 0;
            continue;
        }
        entry(&c);
    }
    if (leaf->rust_store && !stores_seen) rt_exit(10, "decomp selftest: store leaf never wrote");
    unsigned long long calls = atomic_exchange(&leaf->calls, 0);
    unsigned long long differences = atomic_exchange(&leaf->differences, 0);
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
}
