/* Mock game library for tests/test_runtime_boot.py (task-5). Our own code.
 * Stands in for the lifter output: registers hand-written GuestFn entries at fixed guest addresses of the
 * synthetic images the test generates. */
#include "rt_internal.h"

#define MAIN(o) (GUEST_MOD_MAIN + (o))
#define SDK(o) (GUEST_MOD_SDK + (o))
static volatile int g_thread_ran;

static void mock_ctor(Ctx *c) { rt_svc(c, 0x1E); }                     /* runs from main's init_array */

static void mock_thread(Ctx *c) { g_thread_ran = (int)c->x[0]; }     /* thread entry; x0 = arg */

#define GOT(o) (*(uint64_t *)MAIN(o))

/* stands in for nn::init::Start: x0 = handle, x2..x4 = rtld functions */
static void mock_start(Ctx *c) {
    uint64_t run_init = c->x[3];
    uint64_t sp = c->x[31];
    rt_icall(c, run_init);                                                /* rtld runs init_arrays */
    c->x[31] = sp;
    c->x[1] = 18; c->x[2] = 0; c->x[3] = 0;                               /* GetInfo(ProgramId) */
    rt_svc(c, 0x29);
    if (c->x[1] != 0x0100BC0018138000ULL) rt_fatal(c, "mock: wrong program id", c->x[1]);
    c->x[1] = 0x400000; rt_svc(c, 0x01);                                  /* SetHeapSize 4 MB */
    uint64_t heap = c->x[1];
    char *p = (char *)heap;                                               /* heap is committed and writable */
    memcpy(p, "hello", 6);
    /* thread: create, start, wait, check */
    c->x[1] = MAIN(0x400); c->x[2] = 0x1234; c->x[3] = GUEST_STACK_BASE + 0x2000000; c->x[4] = 44; c->x[5] = 0xFFFFFFFE;
    rt_svc(c, 0x08);
    uint32_t th = (uint32_t)c->x[1];
    c->x[0] = th; rt_svc(c, 0x09);
    uint32_t handles[1] = {th};
    c->x[1] = (uint64_t)handles; c->x[2] = 1; c->x[3] = (uint64_t)-1;
    rt_svc(c, 0x18);
    if (!g_thread_ran) rt_fatal(c, "mock: thread did not run", 0);
    /* arbiter: unlock with no waiter writes 0 */
    uint32_t mtx = 0x40000001u;
    c->x[0] = (uint64_t)&mtx; rt_svc(c, 0x1B);
    if (mtx != 0) rt_fatal(c, "mock: unlock did not clear the mutex", mtx);
    /* event */
    rt_svc(c, 0x45);
    uint32_t wh = (uint32_t)c->x[1], rh = (uint32_t)c->x[2];
    c->x[0] = wh; rt_svc(c, 0x11);
    handles[0] = rh; c->x[1] = (uint64_t)handles; c->x[2] = 1; c->x[3] = 0;
    rt_svc(c, 0x18);
    if (c->x[0] != 0) rt_fatal(c, "mock: signaled event not seen", c->x[0]);
    /* an svc we do not implement */
    rt_svc(c, 0x7F);
    /* libc replacement through the GOT slot the relocator filled */
    char dst[8] = {0};
    c->x[0] = (uint64_t)dst; c->x[1] = (uint64_t)"abc"; c->x[2] = 4;
    rt_icall(c, GOT(0x2008));
    if (strcmp(dst, "abc")) rt_fatal(c, "mock: HLE memcpy failed", 0);
    /* C++ throw and catch through the HLE */
    c->x[0] = 8; rt_icall(c, GOT(0x2020));                                /* __cxa_allocate_exception */
    void *obj = (void *)c->x[0];
    c->x[1] = 0; c->x[2] = 0; rt_icall(c, GOT(0x2028));                   /* __cxa_throw */
    if (c->exc != obj) rt_fatal(c, "mock: throw did not set c->exc", 0);
    c->x[0] = (uint64_t)c->exc; rt_icall(c, GOT(0x2030));                 /* __cxa_begin_catch */
    if (c->exc) rt_fatal(c, "mock: begin_catch kept c->exc", 0);
    rt_icall(c, GOT(0x2038));                                             /* __cxa_end_catch */
    /* an nn::os import with no definition: a stub, but not a service request */
    rt_icall(c, GOT(0x2040));
    /* unresolved nn::hid import: logging stub and the service request that ends the boot */
    c->x[0] = 7;
    rt_icall(c, GOT(0x2010));
    rt_log("mock: NOT REACHED after first service request unless --continue");
}

static void reg_mock(void) {
    rt_reg(MAIN(0x100), mock_ctor);
    rt_reg(MAIN(0x400), mock_thread);
    rt_reg(SDK(0x80), mock_start);
}
const RegFn g_reg_chunks[] = {reg_mock};
const uint32_t g_reg_nchunks = 1;
