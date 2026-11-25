/* Star Road Recomp display and GPU driver services (task-13): nvdrv (INvDrvServices) and vi:u (IApplicationRootService).
 * The GPU itself is the NVN layer (runtime/nvn); these are the service sessions the sdk's nn::nv and nn::vi code opens while it sets up
 * graphics. They hand out descriptors and handles and accept the control traffic; nothing here touches the GPU.
 * Command ids are interface facts (switchbrew "NV services", "Display services"). Our own code. */
#include "hle.h"

static void nop(IpcCtx *x) { (void)x; }
static void ret_u32_0(IpcCtx *x) { out_u32(x, 0, 0); }

/* ------------------------------------------------------------------ nvdrv */
enum { NVDEV_NONE = 0, NVDEV_CTRL, NVDEV_CTRL_GPU, NVDEV_GPU, NVDEV_AS_GPU, NVDEV_NVMAP, NVDEV_OTHER };
#define NV_MAX_FD 64
static int g_nv_dev[NV_MAX_FD];
static const char *g_nv_name[NV_MAX_FD];
static CRITICAL_SECTION g_nv_cs;
static INIT_ONCE g_nv_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK nv_init_cb(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&g_nv_cs); return TRUE; }
static void nv_lock(void) { InitOnceExecuteOnce(&g_nv_once, nv_init_cb, 0, 0); EnterCriticalSection(&g_nv_cs); }

static int nv_dev_of(const char *path) {
    if (!strcmp(path, "/dev/nvhost-ctrl")) return NVDEV_CTRL;
    if (!strcmp(path, "/dev/nvhost-ctrl-gpu")) return NVDEV_CTRL_GPU;
    if (!strcmp(path, "/dev/nvhost-gpu")) return NVDEV_GPU;
    if (!strcmp(path, "/dev/nvhost-as-gpu")) return NVDEV_AS_GPU;
    if (!strcmp(path, "/dev/nvmap")) return NVDEV_NVMAP;
    return NVDEV_OTHER;
}
static void nvdrv_open(IpcCtx *x) {
    IpcBuf *b = ipc_in_buf(x, 0);
    char path[96] = {0};
    if (b) memcpy(path, (void *)b->addr, b->size < 95 ? b->size : 95);
    nv_lock();
    int fd = -1;
    for (int i = 1; i < NV_MAX_FD; i++) if (!g_nv_dev[i]) { fd = i; break; }
    if (fd > 0) { g_nv_dev[fd] = nv_dev_of(path); g_nv_name[fd] = strdup(path); }
    LeaveCriticalSection(&g_nv_cs);
    rt_log("nvdrv: Open(\"%s\") -> fd %d", path, fd);
    out_u32(x, 0, fd > 0 ? (uint32_t)fd : 0);
    out_u32(x, 4, fd > 0 ? 0 : 0x0Du);                    /* NvError_BadParameter... out of descriptors */
}
/* /dev/nvmap: handle table for the sdk's nn::nv memory objects (the guest's own code and the movie decoder create handles here; our NVN
 * layer keeps its memory in host allocations, so a handle is just an id with a size). Structs are the Tegra nvmap ioctl ABI. */
#define NVMAP_MAX 4096
static struct { uint32_t size, align; uint8_t used, kind; uint64_t addr; } g_nvmap[NVMAP_MAX];
static uint32_t nvmap_ioctl(uint32_t cmd, uint8_t *io, uint64_t n) {
    uint32_t *w = (uint32_t *)io;
    switch (cmd) {
    case 0xC0080101: {                                   /* CREATE {size, handle} */
        if (n < 8) return 0x16u;
        for (uint32_t h = 1; h < NVMAP_MAX; h++) if (!g_nvmap[h].used) { g_nvmap[h].used = 1; g_nvmap[h].size = w[0]; w[1] = h; return 0; }
        return 0x0Cu;                                    /* NvError_InsufficientMemory */
    }
    case 0xC0080103: case 0xC008010E:                    /* FROM_ID {id, handle}, GET_ID {id, handle}: the id is the handle itself */
        if (n < 8) return 0x16u;
        if (cmd == 0xC008010E) w[0] = w[1]; else w[1] = w[0];
        return 0;
    case 0xC0200104: {                                   /* ALLOC {handle, heap_mask, flags, align, kind, pad[7], address} */
        if (n < 0x20 || w[0] >= NVMAP_MAX || !g_nvmap[w[0]].used) return 0x16u;
        g_nvmap[w[0]].align = w[3]; g_nvmap[w[0]].kind = io[16]; g_nvmap[w[0]].addr = *(uint64_t *)(io + 24);
        return 0;
    }
    case 0xC0180105: {                                   /* FREE {handle, pad, address, size, flags} */
        if (n < 0x18 || w[0] >= NVMAP_MAX || !g_nvmap[w[0]].used) return 0x16u;
        *(uint64_t *)(io + 8) = g_nvmap[w[0]].addr; w[4] = g_nvmap[w[0]].size; w[5] = 0;
        memset(&g_nvmap[w[0]], 0, sizeof g_nvmap[0]);
        return 0;
    }
    case 0xC00C0109: {                                   /* PARAM {handle, param, result}: 1 size, 2 alignment, 3 base, 4 heap, 5 kind */
        if (n < 12 || w[0] >= NVMAP_MAX || !g_nvmap[w[0]].used) return 0x16u;
        switch (w[1]) {
        case 1: w[2] = g_nvmap[w[0]].size; break;
        case 2: w[2] = g_nvmap[w[0]].align; break;
        case 3: w[2] = 0; break;
        case 4: w[2] = 0x40000000u; break;               /* NVMAP_HEAP_IOVMM */
        case 5: w[2] = g_nvmap[w[0]].kind; break;
        default: w[2] = 0; break;
        }
        return 0;
    }
    default: return 0xFFFFFFFFu;                         /* not an nvmap call we know */
    }
}
/* STOPGAP (task-13): with SMRPG_NVDEC_OK=1 the two nvdec ioctls the CRI movie decoder sends right after opening /dev/nvhost-nvdec
 * (0xC0A40009, 0xC1BC0001) answer success and nothing is decoded. Without it they answer NotSupported, which CRI reports through its
 * error callback. Not a decoder; the decoder decision is the owner's. */
static int nvdec_stopgap(void) {
    static int on = -1;
    if (on < 0) on = getenv("SMRPG_NVDEC_OK") != NULL;
    if (on) {
        static volatile LONG logged;
        if (InterlockedIncrement(&logged) <= 4) rt_log("STOPGAP nvdec ioctl answered with success and no decode (SMRPG_NVDEC_OK=1)");
    }
    return on;
}
static void nvdrv_ioctl(IpcCtx *x) {

    uint32_t fd = in_u32(x, 0), cmd = in_u32(x, 4);
    IpcBuf *in = ipc_in_buf(x, 0), *out = ipc_out_buf(x, 0);
    nv_lock();
    int dev = fd < NV_MAX_FD ? g_nv_dev[fd] : 0;
    const char *nm = fd < NV_MAX_FD && g_nv_name[fd] ? g_nv_name[fd] : "?";
    LeaveCriticalSection(&g_nv_cs);
    if (dev == NVDEV_NVMAP) {
        IpcBuf *di = ipc_in_buf(x, ipc_in_data_idx(x)), *dout = ipc_out_buf(x, ipc_out_data_idx(x));
        uint8_t io[64] = {0};
        uint64_t n = di ? di->size : 0;
        if (n > sizeof io) n = sizeof io;
        if (di && n) memcpy(io, (void *)di->addr, (size_t)n);
        nv_lock();
        uint32_t r = nvmap_ioctl(cmd, io, n);
        LeaveCriticalSection(&g_nv_cs);
        if (r != 0xFFFFFFFFu) {
            if (dout && dout->size) ipc_write_out(x, ipc_out_data_idx(x), io, n < dout->size ? n : dout->size);
            out_u32(x, 0, r);
            return;
        }
    }
    if (dev == NVDEV_CTRL && nvdec_stopgap()) {              /* STOPGAP: nvhost-ctrl fence and event ioctls the CRI decoder issues (EventRegister, EventUnregister, EventWait ...) */
        static volatile LONG logged;
        if (InterlockedIncrement(&logged) <= 12) rt_log("STOPGAP nvhost-ctrl ioctl %#x answered with success (SMRPG_NVDEC_OK=1)", cmd);
        out_u32(x, 0, 0);
        return;
    }
    if (cmd == 0x40044801u && dev == NVDEV_OTHER) {        /* nvhost channel SET_NVMAP_FD (nvdec, nvjpg ...): the fd is only remembered by the real driver */
        out_u32(x, 0, 0);
        return;
    }
    if (dev == NVDEV_OTHER && strstr(nm, "nvhost-nv")) {   /* decoder channels (nvdec, nvjpg, nvenc): init-time channel calls succeed; no engine behind them yet */
        IpcBuf *di = ipc_in_buf(x, ipc_in_data_idx(x)), *dout = ipc_out_buf(x, ipc_out_data_idx(x));
        uint8_t io[64] = {0};
        uint64_t n = di ? di->size : 0;
        if (n > sizeof io) n = sizeof io;
        if (di && n) memcpy(io, (void *)di->addr, (size_t)n);
        int known = 1;
        switch (cmd) {
        case 0xC0080002u: *(uint32_t *)(io + 4) = 1 + (fd & 7); break;   /* GET_SYNCPOINT {module, value} */
        case 0xC0080003u: *(uint32_t *)(io + 4) = 0; break;              /* GET_WAITBASE {module, value} */
        case 0x40040007u: case 0x40040009u: case 0x400C0004u: case 0x400C0005u: case 0xC0100001u: break;   /* SET_TIMEOUT, SET_SUBMIT_TIMEOUT, MAP/UNMAP_CMDBUF, SUBMIT */
        case 0xC0A40009u: case 0xC0A4000Au: case 0xC1BC0001u:                              /* nvdec submit / engine ioctls the CRI movie decoder sends */
            known = nvdec_stopgap();                                     /* STOPGAP, SMRPG_NVDEC_OK=1: succeed without decoding anything */
            break;
        default:
            /* nvhost channel Submit (nr 1), MapCommandBuffer (nr 9), UnmapCommandBuffer (nr 10): the ioctl size field varies with the entry count */
            if (((cmd >> 8) & 0xFF) == 0 && ((cmd & 0xFF) == 1 || (cmd & 0xFF) == 9 || (cmd & 0xFF) == 10) && (cmd >> 30) == 3) known = nvdec_stopgap();
            else known = 0;
            break;
        }
        if (known) {
            if (dout && dout->size) ipc_write_out(x, ipc_out_data_idx(x), io, n < dout->size ? n : dout->size);
            out_u32(x, 0, 0);
            return;
        }
    }
    char tag[96];
    snprintf(tag, sizeof tag, "nvdrv:ioctl %s cmd %#x", nm, cmd);

    hle_ipc_note_unknown(tag, cmd);
    rt_log("nvdrv: Ioctl fd %u (%s) cmd %#x in=%llu out=%llu (dev %d): not implemented", fd, nm, cmd, in ? (unsigned long long)in->size : 0ull,
           out ? (unsigned long long)out->size : 0ull, dev);
    out_u32(x, 0, 0x16u);                                 /* NvError_NotSupported */
}
static void nvdrv_close(IpcCtx *x) {
    uint32_t fd = in_u32(x, 0);
    nv_lock();
    if (fd < NV_MAX_FD) g_nv_dev[fd] = 0;
    LeaveCriticalSection(&g_nv_cs);
    out_u32(x, 0, 0);
}
static KObj *g_nv_events[32];
static void nvdrv_query_event(IpcCtx *x) {
    uint32_t fd = in_u32(x, 0), id = in_u32(x, 4);
    KObj **slot = &g_nv_events[(fd + id) & 31];
    if (!*slot) *slot = k_event_named("nv_event", 0);
    out_u32(x, 0, 0);
    out_copy_handle(x, k_handle_for(*slot, 1));
}
CMD_TABLE(g_nvdrv_cmds) = {CMD(0, nvdrv_open), CMD(1, nvdrv_ioctl), CMD(2, nvdrv_close), CMD(3, ret_u32_0), CMD(4, nvdrv_query_event),
                           CMD(6, ret_u32_0), CMD(8, ret_u32_0), CMD(9, nop), CMD(11, nvdrv_ioctl), CMD(12, nvdrv_ioctl), CMD(13, ret_u32_0),
                           CMD(14, ret_u32_0)};
IFACE(g_nvdrv_iface, "nn::nvdrv::INvDrvServices", g_nvdrv_cmds);
static const ServiceDef g_nvdrv_def = {"nvdrv", &g_nvdrv_iface, 0};

/* ------------------------------------------------------------------ vi:u */
#define VI_DISPLAY_ID 1ull
#define VI_BINDER_ID 1u
static KObj *g_vsync_event;
static void vi_open_display(IpcCtx *x) { out_u64(x, 0, VI_DISPLAY_ID); }
static void vi_resolution(IpcCtx *x) { out_u64(x, 0, 1920); out_u64(x, 8, 1080); }
static void vi_vsync_event(IpcCtx *x) {
    if (!g_vsync_event) g_vsync_event = k_event_named("vsync", 0);
    out_copy_handle(x, k_handle_for(g_vsync_event, 1));
}
/* The parcel the sdk reads the layer's binder from: header (payload size, payload offset, objects size, objects offset) then one
 * flat binder object (type, flags, handle, cookie) carrying the interface name "dispdrv". */
static uint64_t vi_native_window_parcel(IpcCtx *x) {
    uint8_t p[0x40] = {0};
    uint32_t *h = (uint32_t *)p;
    h[0] = 0x28; h[1] = 0x10; h[2] = 8; h[3] = 0x38;           /* the object table holds one offset (0): Parcel::readObject only accepts listed objects */
    uint32_t *o = (uint32_t *)(p + 0x10);
    o[0] = 2; o[1] = 0; o[2] = VI_BINDER_ID; o[3] = 0;
    memcpy(p + 0x28, "dispdrv", 8);
    ipc_write_out(x, 0, p, sizeof p);
    return sizeof p;
}
/* nn::vi::native::DeserializeNativeWindow(holder, parcel, size) turns the parcel into an Android BpGraphicBufferProducer on the relay
 * service. Our NVN presents through the host swapchain and never reads the native window, so the holder gets an inert zeroed object
 * (GetNativeWindow returns holder[1] + 0x10) and the call succeeds. The real parse is not run: its binder lookup needs a live buffer
 * queue on the relay, which does not exist here. */
static void vi_window_op(Ctx *c) { c->x[0] = 0; }            /* every ANativeWindow entry point answers success */
HLE_OVR(vi_deserialize_native_window, "_ZN2nn2vi6native23DeserializeNativeWindowEPNS1_18NativeWindowHolderEPKvm") {
    static uint64_t *dummy;
    if (!dummy) {
        dummy = (uint64_t *)VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        uint64_t op = hle_slot("smrpg_vi_window_op", SLOT_HLE, vi_window_op, 0)->addr;
        uint64_t *w = dummy + 2;                                  /* the window sits 0x10 into the holder's object */
        for (int i = 0x40 / 8; i < 0xE0 / 8; i++) w[i] = op;      /* ANativeWindow function pointers (setSwapInterval ... perform) */
    }
    uint64_t *holder = (uint64_t *)c->x[0];
    holder[1] = (uint64_t)dummy;
    rt_log("vi: DeserializeNativeWindow(holder %p, size %llu): inert native window %p", (void *)holder, (unsigned long long)c->x[2], (void *)dummy);
    c->x[0] = 0;
}
static void vi_open_layer(IpcCtx *x) { out_u64(x, 0, vi_native_window_parcel(x)); }
static void vi_create_stray_layer(IpcCtx *x) { out_u64(x, 0, 2); out_u64(x, 8, vi_native_window_parcel(x)); }
/* IHOSBinderDriver (the relay service): our NVN presents through the host swapchain, so the buffer queue transactions are answered
 * with an empty successful parcel and the native handle is a never-signaled event. */
static KObj *g_binder_event;
static void bd_transact(IpcCtx *x) {
    IpcBuf *b = ipc_out_buf(x, 0);
    if (b && b->size) memset((void *)b->addr, 0, b->size);
}
static void bd_native_handle(IpcCtx *x) {
    if (!g_binder_event) g_binder_event = k_event_named("binder", 0);
    out_copy_handle(x, k_handle_for(g_binder_event, 1));
}
CMD_TABLE(g_binder_cmds) = {CMD(0, bd_transact), CMD(1, nop), CMD(2, bd_native_handle), CMD(3, bd_transact)};
IFACE(g_binder_iface, "nn::visrv::IHOSBinderDriver", g_binder_cmds);
static void vi_relay(IpcCtx *x) { out_obj(x, &g_binder_iface, 0); }
CMD_TABLE(g_vi_disp_cmds) = {CMD(100, vi_relay), CMD(1010, vi_open_display), CMD(1011, vi_open_display), CMD(1020, nop), CMD(1102, vi_resolution), CMD(2020, vi_open_layer),
                             CMD(2021, nop), CMD(2030, vi_create_stray_layer), CMD(2031, nop), CMD(2101, nop), CMD(5202, vi_vsync_event)};
IFACE(g_vi_disp_iface, "nn::visrv::IApplicationDisplayService", g_vi_disp_cmds);
static void vi_get_display_service(IpcCtx *x) { out_obj(x, &g_vi_disp_iface, 0); }
CMD_TABLE(g_vi_root_cmds) = {CMD(0, vi_get_display_service), CMD(1, vi_get_display_service)};   /* 1: with proxy name exchange */
IFACE(g_vi_root_iface, "nn::visrv::IApplicationRootService", g_vi_root_cmds);
static const ServiceDef g_vi_def = {"vi:u", &g_vi_root_iface, 0};

void hle_gfxsvc_init(void) {
    hle_register_service(&g_nvdrv_def);
    hle_register_service(&g_vi_def);
}
