/* Star Road Recomp runtime internals (task-5). Our own code. Shared by the runtime sources only. */
#pragma once
#include "rt.h"
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#define RT_ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- log (rt_core.c) ---- */
extern int g_trace;        /* SMRPG_TRACE=1 */
extern int g_continue;     /* --continue: keep running after the first service request */
void rt_log_open(const char *path);
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_log_flush(void);
uint32_t rt_tid(void);                /* small id of the calling guest or host thread */
double rt_now(void);                  /* seconds since process start */

/* ring buffer of the last HLE/svc calls */
void rt_ring_push(const char *kind, const char *name, uint64_t a0, uint64_t a1, uint64_t a2);
void rt_ring_dump(void);

/* ---- results, exit paths ---- */
#define RES_OK            0u
#define RES_INVALID_SIZE  0xCA01u
#define RES_INVALID_ADDR  0xCC01u
#define RES_INVALID_PRIO  0xE001u
#define RES_INVALID_HANDLE 0xE401u
#define RES_TIMEOUT       0xEA01u
#define RES_CANCELLED     0xEC01u
#define RES_INVALID_STATE 0xD401u
#define RES_NOT_IMPL      0xF001u
#define RES_INVALID_ENUM  0xF001u
#define RES_OUT_OF_MEM    0xD001u

void rt_summary(void);                /* print svc and HLE usage; idempotent */
void rt_exit(int code, const char *why) __attribute__((noreturn));
void rt_boot_reached(const char *what, const char *detail) ; /* first service request: log, stop with 0 unless --continue */

/* ---- guest threads and registers ---- */
typedef struct KObj KObj;
typedef struct RtThread {
    uint32_t handle, tid;
    int prio, core;
    uint64_t affinity;
    uint64_t entry, arg, stack_top, stack_base, tlr;
    HANDLE host;
    Ctx *ctx;
    volatile LONG started, exited, cancel_sync;
    KObj *obj;
    uint64_t sync_key;
    /* C++ exception bookkeeping (hle_nn.c) */
    void *caught[16];
    int ncaught;
    char name[24];
} RtThread;
extern _Thread_local Ctx *tl_ctx;
extern uint64_t g_main_thread_handle;
Ctx *rt_ctx_new(RtThread *t);
uint64_t rt_cntvct(void);             /* 19.2 MHz tick from the host clock */
#define RT_CNTFRQ 19200000ULL
void rt_call_guest(Ctx *c, uint64_t addr);   /* x30 = sentinel, rt_icall, uncaught-exception report */
#define RT_SENTINEL (GUEST_HLE_BASE + 0x10)

/* ---- loader (loader.c) ---- */
typedef struct Module {
    const char *name;
    uint64_t base;
    uint32_t image_size, text_size, rodata_off, rodata_size, data_off, data_size;
    uint32_t nsym, nrel, strsz;
    const struct SymRec *syms;
    const char *strtab;
    uint32_t init_off, init_sz, fini_off, fini_sz;
    uint32_t n_unresolved, n_hle, n_wrapped, n_defined;
} Module;
typedef struct SymRec { uint32_t name_off; uint8_t defined, bind, type, pad; uint64_t value, size; } SymRec;
typedef struct RelRec { uint64_t off; uint32_t type, sym; int64_t addend; } RelRec;
#define NMODULES 3
extern Module g_mod[NMODULES];
extern char g_imgdir[512];
int ld_load_all(const char *imgdir);
uint64_t ld_find_export(const char *name);          /* global lookup order main, sdk, subsdk0; 0 if none */
const Module *ld_module_of(uint64_t addr);
void rt_reserve_regions(void);
void rt_fn_tables_init(void);
void rt_fn_commit_module(int m, uint64_t bytes);
void rt_install_veh(void);

/* guest memory regions reported by GetInfo and used by QueryMemory (svc.c) */
#define GUEST_ALIAS_BASE   GUEST_HEAP_BASE
#define GUEST_ALIAS_SIZE   0x400000000ULL              /* 16 GB */
#define GUEST_HEAPRGN_BASE (GUEST_HEAP_BASE + 0x400000000ULL)
#define GUEST_HEAPRGN_SIZE 0x800000000ULL              /* 32 GB */
#define GUEST_STACKRGN_SIZE 0x400000000ULL             /* 16 GB of thread stacks and TLRs */
#define GUEST_ASLR_BASE    0x8000000ULL
#define GUEST_ASLR_SIZE    0x7FF8000000ULL
uint64_t rt_stack_alloc(uint64_t size, uint64_t align);    /* commit from the stack region; 0 on failure */

/* ---- HLE table (hle_nn.c, loader.c) ---- */
typedef void (*HleFn)(Ctx *c);
enum { HLE_F_OVERRIDE = 1 /* beats the sdk/main export of the same name */ };
void hle_add(const char *name, HleFn fn, unsigned flags);
typedef struct HleDef { const char *name; HleFn fn; unsigned flags; } HleDef;
const HleDef *hle_find(const char *name);

typedef enum { SLOT_HLE, SLOT_STUB, SLOT_WRAP, SLOT_INTERNAL } SlotKind;
typedef struct HleSlot {
    const char *name;
    HleFn fn;
    uint64_t addr;       /* synthetic guest address */
    uint64_t real;       /* SLOT_WRAP: lifted target */
    SlotKind kind;
    int nn_service;      /* first call is a FIRST NN SERVICE REQUEST */
    volatile LONG64 count;
    volatile LONG logged;
} HleSlot;
HleSlot *hle_slot(const char *name, SlotKind kind, HleFn fn, uint64_t real);   /* get or create by name */
HleSlot *hle_slot_at(uint64_t guest_addr);
int hle_nslots(void);
HleSlot *hle_slot_n(int i);
int hle_is_nn_service(const char *mangled);
#define HLE_FLAGS(id, sym, flags) \
    static void hle_##id(Ctx *c); \
    __attribute__((constructor)) static void hle_reg_##id(void) { hle_add(sym, hle_##id, flags); } \
    static void hle_##id(Ctx *c)
#define HLE(id, sym) HLE_FLAGS(id, sym, 0)
#define HLE_OVR(id, sym) HLE_FLAGS(id, sym, HLE_F_OVERRIDE)
void hle_thunk(Ctx *c);        /* every HLE slot is registered with this; dispatches on c->pc */
void hle_register_internal(void);

/* ---- svc (svc.c) ---- */
void svc_init(void);
RtThread *svc_main_thread(void);
void svc_thread_entry_run(RtThread *t);
const char *svc_name(uint32_t imm);
extern volatile LONG64 g_svc_count[0x100];
extern volatile LONG g_first_service;
void svc_report_first_service(const char *what, const char *detail);

/* ---- guest string helper (for logs): reads at most n bytes of host memory safely ---- */
const char *rt_safe_str(uint64_t addr, char *out, size_t n);
