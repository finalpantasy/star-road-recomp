/* IR interpreter: the fallback for guest code that was not lifted (dispatch misses, mid-function entries, functions the
 * lifter listed with a reason) and the second implementation checked by the oracle tests. Our own code (task-5).
 * Records come from <imgdir>/ir_<module>.bin: one 32-byte record per text word, same op semantics as the lifted code
 * (ops_gen.h macros, expanded here over the Ctx fields via mode_interp.h). */
#include "rt_internal.h"
#include "ir_ops.h"
#include "mode_interp.h"
#include "ops_gen.h"
#include "eh/eh.h"

typedef struct { uint16_t op; uint8_t f[10]; uint32_t aux; int64_t i[2]; } IRec;
int a64_crypto_exec(v128 *v, uint32_t w);
static const IRec *g_ir[NMODULES];
static uint32_t g_ir_n[NMODULES];
static volatile LONG g_ir_state[NMODULES];   /* 0 unloaded, 1 loading, 2 ready, 3 failed */
static volatile LONG64 g_interp_steps, g_interp_runs;

/* task-14: a call that comes back with c->exc set lands at its LSDA pad (doc 4.4). The throwing callee never ran its
 * epilogue, so the caller's callee-saved registers and sp are restored from a snapshot taken before the call. */
typedef struct { uint64_t x[12]; v128 v[8]; } EhSave;
static inline void eh_save(const Ctx *c, EhSave *s) {
    for (int n = 0; n < 11; n++) s->x[n] = c->x[19 + n];
    s->x[11] = c->x[31];
    for (int n = 0; n < 8; n++) s->v[n] = c->v[8 + n];
}
static int eh_after_call(Ctx *c, uint64_t ra, const EhSave *s, uint64_t *pc) {
    uint64_t pad;
    if (!eh_interp_land(c, ra, &pad)) return 0;
    for (int n = 0; n < 11; n++) c->x[19 + n] = s->x[n];
    c->x[31] = s->x[11];
    for (int n = 0; n < 8; n++) c->v[8 + n] = s->v[n];
    *pc = pad;
    return 1;
}
#define GC_BACK_EDGE(t) do { if ((t) <= pc && __builtin_expect(g_gc_pending != 0, 0)) gc_poll(c); } while (0)

static int ir_load(int m) {
    if (g_ir_state[m] == 2) return 1;
    if (g_ir_state[m] == 3) return 0;
    static volatile LONG lock;
    while (InterlockedExchange(&lock, 1)) Sleep(0);
    if (g_ir_state[m] == 0) {
        char path[640];
        snprintf(path, sizeof path, "%s\\ir_%s.bin", g_imgdir, g_mod[m].name);
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
        const void *p = 0; uint64_t sz = 0;
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li; GetFileSizeEx(f, &li); sz = (uint64_t)li.QuadPart;
            HANDLE mp = CreateFileMappingA(f, 0, PAGE_READONLY, 0, 0, 0);
            if (mp) { p = MapViewOfFile(mp, FILE_MAP_READ, 0, 0, 0); CloseHandle(mp); }
            CloseHandle(f);
        }
        if (p) { g_ir[m] = p; g_ir_n[m] = (uint32_t)(sz / sizeof(IRec)); g_ir_state[m] = 2; rt_log("interpreter: mapped %s (%u records)", path, g_ir_n[m]); }
        else { g_ir_state[m] = 3; rt_log("interpreter: no IR file %s", path); }
    }
    lock = 0;
    return g_ir_state[m] == 2;
}

extern __thread int g_rt_pure;
void interp_run(Ctx *c, uint64_t pc) {
    InterlockedIncrement64(&g_interp_runs);
    uint64_t steps = 0;
    for (;;) {
        int m = (int)((pc >> 28) & 0xF);
        if ((pc >> 32) != 0x71 || m >= NMODULES || !ir_load(m)) { c->pc = pc; rt_fatal(c, "interpreter: pc outside interpretable code", pc); return; }
        uint64_t idx = (pc & (GUEST_MOD_SPAN - 1)) >> 2;
        if (idx >= g_ir_n[m]) { c->pc = pc; rt_fatal(c, "interpreter: pc beyond text", pc); return; }
        const IRec *i = &g_ir[m][idx];
        c->pc = pc;
        steps++;
        switch (i->op) {
#include "interp_gen.inc"
        case IROP_ctl_b: {
            uint64_t t = (uint64_t)i->i[0];
            GuestFn f = g_rt_pure ? 0 : rt_lookup(t);
            if (f) { c->pc = t; f(c); InterlockedAdd64(&g_interp_steps, steps); return; }   /* b to a function start = tail call */
            GC_BACK_EDGE(t);
            pc = t; continue;
        }
        case IROP_ctl_bl: {
            uint64_t t = (uint64_t)i->i[0];
            EhSave sv; eh_save(c, &sv);
            c->x[30] = pc + 4;
            rt_icall(c, t);
            if (c->exc) {
                if (eh_after_call(c, pc + 4, &sv, &pc)) continue;
                InterlockedAdd64(&g_interp_steps, steps); return;
            }
            break;
        }
        case IROP_ctl_blr: {
            uint64_t t = RX(i->f[0]);
            EhSave sv; eh_save(c, &sv);
            c->x[30] = pc + 4;
            rt_icall(c, t);
            if (c->exc) {
                if (eh_after_call(c, pc + 4, &sv, &pc)) continue;
                InterlockedAdd64(&g_interp_steps, steps); return;
            }
            break;
        }
        case IROP_ctl_br: {
            uint64_t t = RX(i->f[0]);
            GuestFn f = g_rt_pure ? 0 : rt_lookup(t);
            if (f) { c->pc = t; f(c); InterlockedAdd64(&g_interp_steps, steps); return; }
            GC_BACK_EDGE(t);
            pc = t; continue;
        }
        case IROP_ctl_ret:
            InterlockedAdd64(&g_interp_steps, steps);
            return;
        case IROP_ctl_bcond:
            if (ir_cond(c->fl, i->f[0])) { GC_BACK_EDGE((uint64_t)i->i[0]); pc = (uint64_t)i->i[0]; continue; }
            break;
        case IROP_ctl_cbz: {
            uint64_t v = RX(i->f[0]);
            if (!i->f[1]) v = (uint32_t)v;
            if ((v == 0) != (i->f[2] != 0)) { GC_BACK_EDGE((uint64_t)i->i[0]); pc = (uint64_t)i->i[0]; continue; }
            break;
        }
        case IROP_ctl_tbz: {
            uint64_t v = RX(i->f[0]);
            if ((((v >> i->f[1]) & 1) != 0) == (i->f[2] != 0)) { GC_BACK_EDGE((uint64_t)i->i[0]); pc = (uint64_t)i->i[0]; continue; }
            break;
        }
        case IROP_ctl_svc:
            c->pc = pc + 4;
            rt_svc(c, (uint32_t)i->i[0]);
            break;
        case IROP_ctl_brk:
            c->pc = pc;
            rt_brk(c, (uint32_t)i->i[0]);
            break;
        default: {
            /* the lifter has no decoder for the cryptographic extension (AES, SHA, PMULL); the raw word is in aux (crypto_a64.c) */
            if (i->op == 0xFFFF && a64_crypto_exec(c->v, i->aux)) break;
            char what[96];
            snprintf(what, sizeof what, "interpreter: %s IR record (op %#x)", i->op == 0xFFFF ? "undecodable" : "unknown", i->op);
            c->pc = pc; rt_fatal(c, what, i->aux); return;
        }
        }
        pc += 4;
    }
}

void interp_stats(uint64_t *runs, uint64_t *steps) { *runs = (uint64_t)g_interp_runs; *steps = (uint64_t)g_interp_steps; }
