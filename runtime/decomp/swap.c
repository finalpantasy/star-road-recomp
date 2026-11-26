#include "rt_internal.h"
#include "swap.h"
#include <stdatomic.h>

typedef int32_t (*IntLeaf)(int32_t, int32_t, int32_t);
typedef struct {
    uint64_t address;
    GuestFn entry, lifted;
    IntLeaf rust;
    atomic_ullong calls, differences;
} Leaf;

#define LEAF(chunk, addr, rust) \
    void f_##addr(Ctx *); \
    extern void f_##addr##_lifted(Ctx *); \
    extern int32_t rust(int32_t, int32_t, int32_t);
#include "leaves.def"
#undef LEAF

enum {
#define LEAF(chunk, addr, rust) index_##addr,
#include "leaves.def"
#undef LEAF
    leaf_count
};

static Leaf leaves[leaf_count] = {
#define LEAF(chunk, addr, rust) {0x##addr##ULL, f_##addr, f_##addr##_lifted, rust, 0, 0},
#include "leaves.def"
#undef LEAF
};

static enum { OFF, ON, AB } mode;

static void run_rust(const Leaf *leaf, Ctx *c) {
    c->x[0] = (uint32_t)leaf->rust((int32_t)c->x[0], (int32_t)c->x[1], (int32_t)c->x[2]);
}

static void compare(Leaf *leaf, Ctx *c) {
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
#include "leaves.def"
#undef LEAF

static uint64_t random_word(uint64_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static void selftest(Leaf *leaf, GuestFn entry) {
    uint64_t seed = leaf->address;
    const uint32_t edges[] = {0, 1, UINT32_MAX, INT32_MAX, 0x80000000u};
    const unsigned nedge = RT_ARRAY_LEN(edges);
    const unsigned edge_cases = nedge * nedge * nedge;
    for (unsigned i = 0; i < 10000 + edge_cases; ++i) {
        Ctx c;
        memset(&c, 0xa5, sizeof c);
        for (unsigned j = 0; j < 32; ++j) c.x[j] = random_word(&seed);
        if (i < edge_cases) {
            unsigned k = i;
            for (unsigned j = 0; j < 3; ++j, k /= nedge)
                c.x[j] = (c.x[j] & 0xffffffff00000000ULL) | edges[k % nedge];
        }
        entry(&c);
    }
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
