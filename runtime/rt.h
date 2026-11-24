/* Star Road Recomp runtime/lifted-code interface (task-5). Our own code.
 * Shared by generated code (lifter output), the IR interpreter and the hand-written runtime. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef long long v128 __attribute__((vector_size(16), aligned(16)));

/* Fixed guest layout (guest address == host address; see docs/RECOMPILER.md 4.1).
 * Module images are linked at fixed bases so the lifter can bake absolute addresses. */
#define GUEST_MOD_MAIN   0x7100000000ULL
#define GUEST_MOD_SDK    0x7110000000ULL
#define GUEST_MOD_SUB0   0x7120000000ULL
#define GUEST_HLE_BASE   0x7130000000ULL /* synthetic addresses of HLE functions, 4 bytes each */
#define GUEST_MOD_SPAN   0x10000000ULL   /* 256 MB per slot */
#define GUEST_HEAP_BASE  0x1000000000ULL /* heap + alias region */
#define GUEST_HEAP_SIZE  0xC00000000ULL  /* 48 GB */
#define GUEST_STACK_BASE 0x7200000000ULL /* thread stacks, TLS regions */

#define RT_NMOD 4 /* 0 main, 1 sdk, 2 subsdk0, 3 hle */

typedef struct Ctx Ctx;
typedef void (*GuestFn)(Ctx *c);

struct Ctx {
    uint64_t x[32];   /* x[31] is sp */
    v128 v[32];
    uint64_t pc;      /* dispatch target (rt_dispatch), fault pc */
    uint8_t fl[4];    /* N Z C V; only valid in the interpreter and at test boundaries */
    uint32_t fpcr, fpsr;
    uint64_t tpidr_el0, tpidrro_el0;
    uint64_t excl_addr, excl_val;
    uint32_t excl_size, excl_valid;
    void *exc;        /* pending C++ exception object, 0 if none (unwind by return, doc 4.4) */
    uint64_t exc_type;
    struct RtThread *thr; /* runtime-owned per-thread data */
    uint64_t scratch[8];
};

/* ---- provided by the runtime (runtime/*.c) ---- */
extern uint32_t *g_fn[RT_NMOD];        /* dense tables: (addr - base) >> 2 -> uint32 offset from __ImageBase, 0 = none */
void rt_reg(uint64_t guest_addr, GuestFn f);   /* generated reg_<chunk>() and HLE registration (HLE addresses are module slot 3) */
void rt_icall(Ctx *c, uint64_t target);        /* guest `blr`: args already in ctx; miss -> interpreter + profile */
void rt_dispatch(Ctx *c);                      /* guest indirect jump / tail call to c->pc */
void rt_svc(Ctx *c, uint32_t imm);             /* all registers flushed before, x0..x7 reloaded after */
void rt_brk(Ctx *c, uint32_t imm);
void rt_poll(Ctx *c);                          /* GC / suspend safepoint (loop back-edge) */
void rt_fatal(Ctx *c, const char *what, uint64_t arg);

/* ---- provided by runtime/interp.c (our IR interpreter, same op semantics as the lifted code) ---- */
void interp_run(Ctx *c, uint64_t pc);          /* run guest code from pc until it returns to the caller's x30 sentinel */

/* ---- generated: per-chunk registration, called by rt_init ---- */
typedef void (*RegFn)(void);
extern const RegFn g_reg_chunks[];
extern const uint32_t g_reg_nchunks;

static inline GuestFn rt_lookup(uint64_t a) {
    extern char __ImageBase;
    uint64_t m = (a >> 28) & 0xF;
    if ((a >> 32) != 0x71 || m >= RT_NMOD) return 0;
    uint32_t off = g_fn[m][(a & (GUEST_MOD_SPAN - 1)) >> 2];
    return off ? (GuestFn)(&__ImageBase + off) : 0;
}
