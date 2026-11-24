/* Oracle test runner (task-5, AC2). Our own code.
 * Executes single-instruction cases through (a) the IR interpreter ops and (b) lifted C functions,
 * on identical states and memory, and writes both results for comparison with Unicorn in oracle.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <setjmp.h>
#include "ir_ops.h"
#include "mode_interp.h"
#include "ops_gen.h"

#define MEM_BASE 0x20000000ULL
#define MEM_SIZE 0x40000
#define PC_ADDR (MEM_BASE + 0x20000)

typedef struct { uint16_t op; uint8_t f[10]; uint32_t aux; int64_t i[2]; } IRec;
typedef struct { uint64_t x[32]; uint8_t v[32][16]; uint8_t fl[4]; uint32_t fpcr, fpsr; uint64_t tpidr, tpidrro; } State;
typedef struct { IRec rec; uint32_t fn_index; uint32_t word; State st; } CaseIn;
#define NDIFF 128
typedef struct { State si, sl; uint32_t ndi, ndl; uint32_t di[NDIFF], dl[NDIFF]; } CaseOut;

extern void (*const t_fns[])(Ctx *);

uint64_t rt_cntvct(void) { return 123456789ull; }
void rt_fatal(Ctx *c, const char *what, uint64_t arg) { (void)c; fprintf(stderr, "fatal %s %llx\n", what, (unsigned long long)arg); }
void rt_icall(Ctx *c, uint64_t t) { (void)c; (void)t; }
void rt_dispatch(Ctx *c) { (void)c; }
void rt_svc(Ctx *c, uint32_t i) { (void)c; (void)i; }
void rt_brk(Ctx *c, uint32_t i) { (void)c; (void)i; }

static jmp_buf g_jb;
static volatile int g_armed;
static LONG CALLBACK veh(PEXCEPTION_POINTERS p) {
    if (g_armed && p->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) { g_armed = 0; longjmp(g_jb, 1); }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void interp_exec(Ctx *c, const IRec *i) {
    switch (i->op) {
#include "interp_gen.inc"
    default: fprintf(stderr, "bad op %u\n", i->op); break;
    }
}

static void st_to_ctx(const State *s, Ctx *c) {
    memset(c, 0, sizeof *c);
    memcpy(c->x, s->x, sizeof c->x);
    for (int k = 0; k < 32; k++) memcpy(&c->v[k], s->v[k], 16);
    memcpy(c->fl, s->fl, 4);
    c->fpcr = s->fpcr; c->fpsr = s->fpsr; c->tpidr_el0 = s->tpidr; c->tpidrro_el0 = s->tpidrro;
}
static void ctx_to_st(const Ctx *c, State *s) {
    memset(s, 0, sizeof *s);
    memcpy(s->x, c->x, sizeof s->x);
    for (int k = 0; k < 32; k++) memcpy(s->v[k], &c->v[k], 16);
    memcpy(s->fl, c->fl, 4);
    s->fpcr = c->fpcr; s->fpsr = c->fpsr; s->tpidr = c->tpidr_el0; s->tpidrro = c->tpidrro_el0;
}
static uint32_t diff_mem(const uint8_t *a, const uint8_t *b, uint32_t *out) {
    uint32_t n = 0;
    for (uint32_t k = 0; k < MEM_SIZE; k++)
        if (a[k] != b[k] && n < NDIFF) out[n++] = (k << 8) | b[k];
    return n;
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    uint8_t *mem = VirtualAlloc((void *)MEM_BASE, MEM_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (mem != (uint8_t *)MEM_BASE) { fprintf(stderr, "cannot map test memory\n"); return 3; }
    static uint8_t img[MEM_SIZE], pristine[MEM_SIZE];
    FILE *f = fopen(argv[2], "rb"); if (!f || fread(img, 1, MEM_SIZE, f) != MEM_SIZE) return 4; fclose(f);
    f = fopen(argv[1], "rb"); if (!f) return 5;
    uint32_t hdr[2]; if (fread(hdr, 4, 2, f) != 2) return 6;
    uint32_t n = hdr[1];
    AddVectoredExceptionHandler(1, veh);
    FILE *o = fopen(argv[3], "wb");
    for (uint32_t k = 0; k < n; k++) {
        CaseIn ci; if (fread(&ci, sizeof ci, 1, f) != 1) return 7;
        CaseOut co; memset(&co, 0, sizeof co);
        memcpy(pristine, img, MEM_SIZE);
        memcpy(pristine + (PC_ADDR - MEM_BASE), &ci.word, 4);
        Ctx c1, c2;
        memcpy(mem, pristine, MEM_SIZE); st_to_ctx(&ci.st, &c1);
        if (setjmp(g_jb) == 0) { g_armed = 1; interp_exec(&c1, &ci.rec); g_armed = 0; ctx_to_st(&c1, &co.si); co.ndi = diff_mem(pristine, mem, co.di); }
        else co.ndi = 0xFFFFFFFFu;
        memcpy(mem, pristine, MEM_SIZE); st_to_ctx(&ci.st, &c2);
        if (setjmp(g_jb) == 0) { g_armed = 1; t_fns[ci.fn_index](&c2); g_armed = 0; ctx_to_st(&c2, &co.sl); co.ndl = diff_mem(pristine, mem, co.dl); }
        else co.ndl = 0xFFFFFFFFu;
        fwrite(&co, sizeof co, 1, o);
    }
    fclose(o); fclose(f);
    return 0;
}
