/* Star Road Recomp Horizon service HLE (task-13): shared header for runtime/hle/. Our own code.
 *
 * Design (docs/RECOMPILER.md 4.6): the sdk's own nn::sf / nn::fs / nn::oe code is lifted and runs unchanged. It talks to
 * services through svcConnectToNamedPort and svcSendSyncRequest with HIPC/CMIF messages in the thread local region. This
 * layer is the server side: it parses the message, finds the interface object behind the session handle, runs the command
 * handler and writes the response. Guest pointers are host pointers (identity mapping), so buffers are used in place. */
#pragma once
#include "../rt_internal.h"

/* ------------------------------------------------------------------ results */
#define MAKE_RES(mod, desc) ((uint32_t)((mod) | ((desc) << 9)))
#define RES_SM_NOT_REGISTERED  MAKE_RES(21, 7)     /* 0xE15 */
#define RES_SM_INVALID_NAME    MAKE_RES(21, 6)
#define RES_SM_NOT_ALLOWED     MAKE_RES(21, 8)     /* 0x1015: not in the service access list */
#define RES_FS_PATH_NOT_FOUND  MAKE_RES(2, 1)      /* 0x202 */
#define RES_FS_PATH_EXISTS     MAKE_RES(2, 2)      /* 0x402 */
#define RES_FS_TARGET_LOCKED   MAKE_RES(2, 7)
#define RES_FS_INVALID_ARG     MAKE_RES(2, 6002)
#define RES_FS_OUT_OF_RANGE    MAKE_RES(2, 3005)
#define RES_FS_NOT_FOUND_SAVE  MAKE_RES(2, 1002)   /* save data not found */
#define RES_FS_UNSUPPORTED     MAKE_RES(2, 6001)
#define RES_CMIF_UNKNOWN_CMD   MAKE_RES(10, 221)   /* unknown command id (logged, never expected on the boot path) */
#define RES_CMIF_BAD_OBJECT    MAKE_RES(10, 301)
#define RES_UNSUPPORTED        MAKE_RES(10, 1)

/* ------------------------------------------------------------------ kernel objects (svc.c) */
typedef struct KObj KObj;
uint32_t k_session_new(const char *name, void *user);      /* handle (one ref); user is the IPC Session */
void *k_session_user(uint32_t handle);                       /* 0 if not a session handle */
KObj *k_event_named(const char *name, int signaled);        /* same, with a name for the trace */
KObj *k_event_new(int signaled);                             /* object with no handle yet */
uint32_t k_handle_for(KObj *o, int role);                    /* new handle to o (events: role 0 writable, 1 readable) */
void k_event_set(KObj *o, int signaled);
int k_event_get(KObj *o);
KObj *k_shmem_new(uint64_t size);                            /* backing allocated; mapped by svcMapSharedMemory */
void *k_shmem_ptr(KObj *o);                                  /* where the service should write: first guest mapping, else backing */
uint64_t k_shmem_size(KObj *o);
KObj *k_obj_of(uint32_t handle);                             /* 0 if invalid */
int k_obj_type(KObj *o);
uint64_t k_transfer_addr(KObj *o);                           /* transfer memory: guest address and size */
uint64_t k_transfer_size(KObj *o);
void hle_session_closed(void *user);                         /* called by svcCloseHandle on a session */

/* ------------------------------------------------------------------ IPC */
typedef struct { uint64_t addr, size; uint32_t idx; } IpcBuf;

struct IpcCtx;
typedef void (*SvcCmdFn)(struct IpcCtx *x);
typedef struct SvcCmd { uint32_t id; const char *name; SvcCmdFn fn; } SvcCmd;
typedef struct SvcIface {
    const char *name;
    const SvcCmd *cmds;
    int ncmds;
    void (*destroy)(void *state);
} SvcIface;

typedef struct SvcObj {
    const SvcIface *iface;
    void *state;
    volatile LONG refs;
} SvcObj;

typedef struct Session Session;

#define IPC_MAXBUF 16
typedef struct IpcCtx {
    Ctx *c;
    Session *sess;
    SvcObj *self;
    uint32_t cmd, type;
    /* request */
    const uint8_t *in; uint32_t in_size;                 /* payload after the CMIF header */
    IpcBuf px[IPC_MAXBUF]; int npx;                      /* X: pointer descriptors (client to server) */
    IpcBuf pa[IPC_MAXBUF]; int npa;                      /* A: send buffers */
    IpcBuf pb[IPC_MAXBUF]; int npb;                      /* B: receive buffers */
    IpcBuf pw[IPC_MAXBUF]; int npw;                      /* W: exchange buffers */
    IpcBuf pc[IPC_MAXBUF]; int npc;                      /* C: receive list */
    uint32_t copy_h[16]; int ncopy;
    uint32_t move_h[16]; int nmove;
    uint64_t pid; int has_pid;
    uint32_t in_objs[8]; int nin_objs;                   /* domain object ids passed as arguments */
    /* response */
    uint32_t result;
    uint8_t out[0x180]; uint32_t out_size;
    uint32_t r_copy[8]; int r_ncopy;
    uint32_t r_move[8]; int r_nmove;
    struct { const SvcIface *iface; void *state; } r_obj[8]; int r_nobj;
    IpcBuf r_x[IPC_MAXBUF]; int r_nx;                    /* X descriptors in the response (out pointers) */
    int is_domain;
} IpcCtx;

/* argument access: offsets are into the payload (after the 16 byte CMIF header) */
static inline uint32_t in_u32(IpcCtx *x, uint32_t off) { uint32_t v = 0; if (off + 4 <= x->in_size) memcpy(&v, x->in + off, 4); return v; }
static inline uint64_t in_u64(IpcCtx *x, uint32_t off) { uint64_t v = 0; if (off + 8 <= x->in_size) memcpy(&v, x->in + off, 8); return v; }
static inline const uint8_t *in_ptr(IpcCtx *x, uint32_t off) { return x->in + off; }
static inline void out_bytes(IpcCtx *x, uint32_t off, const void *p, uint32_t n) {
    if (off + n > sizeof x->out) return;
    memcpy(x->out + off, p, n);
    if (off + n > x->out_size) x->out_size = off + n;
}
static inline void out_u32(IpcCtx *x, uint32_t off, uint32_t v) { out_bytes(x, off, &v, 4); }
static inline void out_u64(IpcCtx *x, uint32_t off, uint64_t v) { out_bytes(x, off, &v, 8); }
static inline void out_zero(IpcCtx *x, uint32_t off, uint32_t n) { if (off + n <= sizeof x->out) { memset(x->out + off, 0, n); if (off + n > x->out_size) x->out_size = off + n; } }

void out_obj(IpcCtx *x, const SvcIface *iface, void *state);   /* return a new interface object (move handle or domain id) */
void out_copy_handle(IpcCtx *x, uint32_t h);
void out_move_handle(IpcCtx *x, uint32_t h);
/* i-th input buffer: map-alias (A) buffers first, then pointer (X); out buffer: B first, then the C list */
SvcObj *ipc_in_obj(IpcCtx *x, int i);      /* i-th interface object argument, 0 if none */
IpcBuf *ipc_in_buf(IpcCtx *x, int i);
IpcBuf *ipc_out_buf(IpcCtx *x, int i);
uint64_t ipc_write_out(IpcCtx *x, int i, const void *data, uint64_t n);   /* writes into out buffer i, registers C-list size, returns bytes written */
int ipc_in_data_idx(IpcCtx *x);    /* index (for ipc_in_buf) of the input buffer that carries bytes; auto-select pairs have one empty half */
int ipc_out_data_idx(IpcCtx *x);   /* same for ipc_out_buf */

/* services and objects */
SvcObj *svcobj_new(const SvcIface *iface, void *state);
void svcobj_release(SvcObj *o);
typedef struct ServiceDef { const char *name; const SvcIface *root; void *(*make_state)(void); } ServiceDef;
void hle_register_service(const ServiceDef *d);               /* named sm: services, looked up by sm GetService */
const ServiceDef *hle_find_service(const char *name);

/* the svc.c hooks */
void hle_ipc_connect_named_port(Ctx *c, const char *name);
void hle_ipc_send_sync(Ctx *c, uint32_t handle);
int hle_permissive(void);
void hle_ipc_summary(void);                                    /* coverage printed at exit */
void hle_ipc_note_unknown(const char *iface, uint32_t cmd);

#define CMD_TABLE(var) static const SvcCmd var[]
#define CMD(id, fn) {(id), #fn, fn}
#define IFACE(var, nm, table) static const SvcIface var = {nm, table, (int)(sizeof(table) / sizeof(table[0])), 0}
#define IFACE_D(var, nm, table, dtor) static const SvcIface var = {nm, table, (int)(sizeof(table) / sizeof(table[0])), dtor}
/* a service object with no state and an empty command table still answers; unknown commands are logged */

/* ------------------------------------------------------------------ small helpers shared by services */
extern uint64_t g_hle_applet_resource_user_id;
void hle_log_ipc(IpcCtx *x, const char *what);
const char *hle_data_root(void);             /* A:\AI\Recomps\smrpg-work (SMRPG_WORK overrides) */
const char *hle_romfs_dir(void);             /* extracted RomFS: <work>\v101\romfs */
const char *hle_save_dir(void);              /* save data root on A: */

/* registration of each service module: called once at startup */
void hle_services_init(void);
void hle_sm_init(void);
void hle_fs_init(void);
void hle_applet_init(void);
void hle_misc_init(void);
void hle_hid_init(void);
void hle_audio_init(void);
void hle_time_init(void);
void hle_sys_init(void);
void hle_gfxsvc_init(void);
void hle_net_init(void);

/* the level0 marker: the first read of the game's first scene through the RomFS (set by hle_fs.c) */
extern volatile LONG g_level0_reached;
void hle_boot_marker(const char *what);
