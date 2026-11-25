/* Star Road Recomp applet and performance services (task-13): appletOE (IApplicationProxy and its sub interfaces) and apm.
 * The sdk's nn::oe code runs lifted and talks to these. Command ids are interface facts (switchbrew "Applet Manager services");
 * the behavior is what an application that was just launched from the home menu sees: in focus, docked, no launch parameter. */
#include "hle.h"

#define RES_AM_NO_MESSAGES    MAKE_RES(128, 3)     /* 0x680 */
#define RES_AM_NOT_AVAILABLE  MAKE_RES(128, 1)     /* no launch parameter / storage */
#define RES_AM_NO_DATA        MAKE_RES(128, 2)     /* 0x480: nothing in the launch parameter channel (the sdk treats this as "none queued") */

static void nop(IpcCtx *x) { (void)x; }
static void ret_true(IpcCtx *x) { out_bytes(x, 0, &(uint8_t){1}, 1); }
static void ret_false(IpcCtx *x) { out_bytes(x, 0, &(uint8_t){0}, 1); }
static void ret_u32_0(IpcCtx *x) { out_u32(x, 0, 0); }
static void ret_u64_0(IpcCtx *x) { out_u64(x, 0, 0); }
static void ret_u32_1(IpcCtx *x) { out_u32(x, 0, 1); }
static void ret_f32_1(IpcCtx *x) { float f = 1.0f; out_bytes(x, 0, &f, 4); }

/* events handed to the guest as copy handles; one object per kind, many handles */
static KObj *g_msg_event, *g_res_event, *g_launchable_event, *g_sleeplock_event, *g_gpuerr_event, *g_misc_event;
static KObj *ev(KObj **slot, const char *name) {
    if (!*slot) *slot = k_event_named(name, 0);
    return *slot;
}
static void ret_event_n(IpcCtx *x, KObj **slot, const char *name) { out_copy_handle(x, k_handle_for(ev(slot, name), 1)); }
#define ret_event(x, slot) ret_event_n(x, slot, #slot + 3)

/* ------------------------------------------------------------------ ICommonStateGetter */
/* AM message queue (switchbrew "AppletMessage"): a freshly launched application is told once that it gained focus (15, FocusStateChanged);
 * the sdk's nn::oe::Initialize blocks on the message event until then. The event stays signaled while the queue is non-empty. */
#define AM_MSG_FOCUS_STATE_CHANGED 15
static volatile int g_msgq[8], g_msgq_n, g_msgq_init;
static void csg_get_event(IpcCtx *x) {
    int first = !g_msg_event;
    ret_event(x, &g_msg_event);
    if (first && InterlockedCompareExchange((volatile LONG *)&g_msgq_init, 1, 0) == 0) {
        g_msgq[g_msgq_n++] = AM_MSG_FOCUS_STATE_CHANGED;
        k_event_set(g_msg_event, 1);
    }
}
static void csg_receive_message(IpcCtx *x) {
    if (g_msgq_n <= 0) { x->result = RES_AM_NO_MESSAGES; return; }
    uint32_t m = (uint32_t)g_msgq[0];
    for (int i = 1; i < g_msgq_n; i++) g_msgq[i - 1] = g_msgq[i];
    g_msgq_n--;
    if (g_msgq_n == 0 && g_msg_event) k_event_set(g_msg_event, 0);
    out_u32(x, 0, m);
}
static void csg_operation_mode(IpcCtx *x) { out_bytes(x, 0, &(uint8_t){1}, 1); }   /* 1 = docked (console) */
static void csg_perf_mode(IpcCtx *x) { out_u32(x, 0, 1); }                          /* PerformanceMode 1 = docked */
static void csg_focus_state(IpcCtx *x) { out_bytes(x, 0, &(uint8_t){1}, 1); }      /* 1 = InFocus */
static void csg_sleeplock_event(IpcCtx *x) { ret_event(x, &g_sleeplock_event); }
static void csg_default_resolution(IpcCtx *x) { out_u32(x, 0, 1920); out_u32(x, 4, 1080); }
static void csg_resolution_event(IpcCtx *x) { ret_event(x, &g_res_event); }
static void csg_region(IpcCtx *x) { out_bytes(x, 0, &(uint8_t){1}, 1); }
CMD_TABLE(g_csg_cmds) = {
    CMD(0, csg_get_event), CMD(1, csg_receive_message), CMD(5, csg_operation_mode), CMD(6, csg_perf_mode), CMD(8, ret_false),
    CMD(9, csg_focus_state), CMD(10, nop), CMD(13, csg_sleeplock_event), CMD(50, ret_false), CMD(51, nop), CMD(52, nop), CMD(53, nop),
    CMD(54, nop), CMD(60, csg_default_resolution), CMD(61, csg_resolution_event), CMD(62, ret_u32_0), CMD(66, nop), CMD(91, ret_u32_0),
    CMD(300, csg_region), CMD(900, nop)};
IFACE(g_csg_iface, "nn::am::ICommonStateGetter", g_csg_cmds);

/* ------------------------------------------------------------------ ISelfController */
/* a console with nothing running in front lets the application open a library applet at once: signaled from the start */
static void sc_launchable_event(IpcCtx *x) {
    int first = !g_launchable_event;
    ret_event(x, &g_launchable_event);
    if (first) k_event_set(g_launchable_event, 1);
}
static void sc_create_layer(IpcCtx *x) { out_u64(x, 0, 1); }                       /* managed layer id */
static void sc_create_separable_layer(IpcCtx *x) { out_u64(x, 0, 1); out_u64(x, 8, 2); } /* display layer id, recording layer id */
static void sc_suspended_tick(IpcCtx *x) { out_u64(x, 0, 0); }
static void sc_tick_event(IpcCtx *x) { ret_event(x, &g_misc_event); }
static void sc_illuminance_ex(IpcCtx *x) { float f = 0.0f; out_bytes(x, 0, &(uint8_t){0}, 1); out_bytes(x, 4, &f, 4); }
CMD_TABLE(g_sc_cmds) = {
    CMD(0, nop), CMD(1, nop), CMD(2, nop), CMD(3, nop), CMD(4, nop), CMD(9, sc_launchable_event), CMD(10, nop), CMD(11, nop), CMD(12, nop),
    CMD(13, nop), CMD(14, nop), CMD(16, nop), CMD(19, nop), CMD(40, sc_create_layer), CMD(41, ret_false), CMD(44, sc_create_separable_layer), CMD(50, nop), CMD(62, nop),
    CMD(63, ret_u32_0), CMD(65, nop), CMD(67, ret_false), CMD(68, nop), CMD(69, ret_false), CMD(71, sc_illuminance_ex), CMD(80, nop),
    CMD(90, sc_suspended_tick), CMD(91, sc_tick_event), CMD(100, nop), CMD(120, nop), CMD(130, nop)};
IFACE(g_sc_iface, "nn::am::ISelfController", g_sc_cmds);

/* ------------------------------------------------------------------ IWindowController, IAudioController, IDisplayController */
static void wc_resource_user_id(IpcCtx *x) { out_u64(x, 0, g_hle_applet_resource_user_id); }
CMD_TABLE(g_wc_cmds) = {CMD(0, nop), CMD(1, wc_resource_user_id), CMD(10, nop)};
IFACE(g_wc_iface, "nn::am::IWindowController", g_wc_cmds);

CMD_TABLE(g_ac_cmds) = {CMD(0, nop), CMD(1, ret_f32_1), CMD(2, ret_f32_1), CMD(3, nop), CMD(4, nop)};
IFACE(g_ac_iface, "nn::am::IAudioController", g_ac_cmds);

CMD_TABLE(g_dc_cmds) = {CMD(8, nop), CMD(11, nop), CMD(15, nop)};
IFACE(g_dc_iface, "nn::am::IDisplayController", g_dc_cmds);

/* ------------------------------------------------------------------ ILibraryAppletCreator, IDebugFunctions */
/* Library applets (the controller support applet is the one the game opens): a console with one connected controller answers
 * a ShowControllerSupport request at once with one player and success. Protocol (switchbrew "Applet Manager services"):
 * ILibraryAppletCreator.CreateLibraryApplet(id, mode) gives an ILibraryAppletAccessor; the caller pushes IStorage objects with
 * its arguments, calls Start, waits for the state event, then pops the reply IStorage. Storages are plain byte buffers. */
typedef struct { uint8_t *data; int64_t size; } Storage;
static Storage *storage_new(int64_t size) {
    Storage *s = calloc(1, sizeof *s);
    if (size < 0) size = 0;
    s->data = calloc(1, (size_t)size + 1);
    s->size = size;
    return s;
}
static void sta_get_size(IpcCtx *x) { Storage *s = x->self->state; out_u64(x, 0, (uint64_t)(s ? s->size : 0)); }
static void sta_write(IpcCtx *x) {
    Storage *s = x->self->state;
    int64_t off = (int64_t)in_u64(x, 0);
    IpcBuf *b = ipc_in_buf(x, ipc_in_data_idx(x));
    if (!s || !b || off < 0 || off + (int64_t)b->size > s->size) { x->result = MAKE_RES(128, 502); return; }   /* out of range */
    memcpy(s->data + off, (const void *)(uintptr_t)b->addr, (size_t)b->size);
}
static void sta_read(IpcCtx *x) {
    Storage *s = x->self->state;
    int64_t off = (int64_t)in_u64(x, 0);
    int bi = ipc_out_data_idx(x);
    IpcBuf *b = ipc_out_buf(x, bi);
    if (!s || !b || off < 0) { x->result = MAKE_RES(128, 502); return; }
    uint64_t n = b->size;
    if (off + (int64_t)n > s->size) n = off < s->size ? (uint64_t)(s->size - off) : 0;
    ipc_write_out(x, bi, s->data + off, n);
}
CMD_TABLE(g_sta_cmds) = {CMD(0, sta_get_size), CMD(10, sta_write), CMD(11, sta_read)};
IFACE(g_sta_iface, "nn::am::IStorageAccessor", g_sta_cmds);
static void st_open(IpcCtx *x) { out_obj(x, &g_sta_iface, x->self->state); }
CMD_TABLE(g_st_cmds) = {CMD(0, st_open)};
IFACE(g_st_iface, "nn::am::IStorage", g_st_cmds);

#define LA_ID_CONTROLLER 0x0C   /* libnx AppletId_LibraryAppletController */
typedef struct { uint32_t id, mode; KObj *state_ev, *out_ev; int started; Storage *out; } LibApplet;
static void la_state_event(IpcCtx *x) { LibApplet *a = x->self->state; out_copy_handle(x, k_handle_for(a->state_ev, 1)); }
static void la_is_completed(IpcCtx *x) { LibApplet *a = x->self->state; out_bytes(x, 0, &(uint8_t){(uint8_t)a->started}, 1); }
static void la_start(IpcCtx *x) {
    LibApplet *a = x->self->state;
    /* reply of the controller applet (libnx HidLaControllerSupportResultInfoInternal): s8 player count, u32 selected npad id, u32 result */
    a->out = storage_new(0x10);
    a->out->data[0] = 1;                       /* one player */
    uint32_t selected = 0;                     /* NpadId No1 (the connected controller) */
    memcpy(a->out->data + 4, &selected, 4);    /* result 0 at +8 */
    a->started = 1;
    k_event_set(a->out_ev, 1);
    k_event_set(a->state_ev, 1);               /* the applet ran to completion */
}
static void la_push_in(IpcCtx *x) {
    SvcObj *o = ipc_in_obj(x, 0);
    Storage *s = o ? o->state : 0;
    rt_log("applet %u: PushInData %lld bytes", ((LibApplet *)x->self->state)->id, (long long)(s ? s->size : -1));
}
static void la_pop_out(IpcCtx *x) {
    LibApplet *a = x->self->state;
    if (!a->out) { x->result = RES_AM_NOT_AVAILABLE; return; }
    out_obj(x, &g_st_iface, a->out);
    a->out = 0;
}
static void la_get_result(IpcCtx *x) { (void)x; }
static void la_out_event(IpcCtx *x) { LibApplet *a = x->self->state; out_copy_handle(x, k_handle_for(a->out_ev, 1)); }
static void la_info(IpcCtx *x) { LibApplet *a = x->self->state; out_u32(x, 0, a->id); out_u32(x, 4, a->mode); }
CMD_TABLE(g_la_cmds) = {
    CMD(0, la_state_event), CMD(1, la_is_completed), CMD(10, la_start), CMD(20, nop), CMD(25, nop), CMD(30, la_get_result), CMD(50, nop),
    CMD(100, la_push_in), CMD(101, la_pop_out), CMD(102, nop), CMD(103, nop), CMD(105, la_out_event), CMD(106, la_out_event), CMD(110, ret_false),
    CMD(120, la_info), CMD(150, nop)};
IFACE(g_la_iface, "nn::am::ILibraryAppletAccessor", g_la_cmds);

static void lac_create(IpcCtx *x) {
    LibApplet *a = calloc(1, sizeof *a);
    a->id = in_u32(x, 0);
    a->mode = in_u32(x, 4);
    a->state_ev = k_event_named("libapplet_state", 0);
    a->out_ev = k_event_named("libapplet_out", 0);
    rt_log("applet: CreateLibraryApplet id=0x%x mode=%u%s", a->id, a->mode, a->id == LA_ID_CONTROLLER ? " (controller support)" : "");
    out_obj(x, &g_la_iface, a);
}
static void lac_create_storage(IpcCtx *x) { out_obj(x, &g_st_iface, storage_new((int64_t)in_u64(x, 0))); }
CMD_TABLE(g_lac_cmds) = {CMD(0, lac_create), CMD(10, lac_create_storage)};
IFACE(g_lac_iface, "nn::am::ILibraryAppletCreator", g_lac_cmds);
CMD_TABLE(g_dbg_cmds) = {CMD(0xFFFFFFF0u, nop)};
IFACE(g_dbg_iface, "nn::am::IDebugFunctions", g_dbg_cmds);

/* ------------------------------------------------------------------ IApplicationFunctions */
/* Launched from the home menu with one user selected: kind 2 (PreselectedUser) holds a 0x88 byte storage, magic 0xC79497CA, a
 * "selected" byte and the 16 byte user id (the same id acc:u0 reports); no other launch parameter is queued. */
static void af_pop_launch_param(IpcCtx *x) {
    rt_log("applet: PopLaunchParameter kind=%u", in_u32(x, 0));
    if (in_u32(x, 0) != 2) { x->result = RES_AM_NO_DATA; return; }
    Storage *s = storage_new(0x88);
    uint32_t magic = 0xC79497CAu;
    memcpy(s->data, &magic, 4);
    s->data[4] = 1;
    s->data[8] = 1;                            /* user id {1, 0, ...} */
    out_obj(x, &g_st_iface, s);
}
static void af_set_terminate_result(IpcCtx *x) { rt_log("applet: SetTerminateResult 0x%x", in_u32(x, 0)); }
static void af_ensure_save(IpcCtx *x) { out_u64(x, 0, 0); }                          /* the save area lives under hle_save_dir() */
static void af_desired_language(IpcCtx *x) { out_u64(x, 0, 0x00000053552D6E65ull); } /* "en-US" */
static void af_display_version(IpcCtx *x) { char v[16] = "1.0.1"; out_bytes(x, 0, v, 16); }
static void af_save_size(IpcCtx *x) { out_u64(x, 0, 0x4000000); out_u64(x, 8, 0x4000000); }
static void af_uuid(IpcCtx *x) { out_zero(x, 0, 16); }
static void af_gpu_error_event(IpcCtx *x) { ret_event(x, &g_gpuerr_event); }
static void af_misc_event(IpcCtx *x) { ret_event(x, &g_misc_event); }
CMD_TABLE(g_af_cmds) = {
    CMD(1, af_pop_launch_param), CMD(20, af_ensure_save), CMD(21, af_desired_language), CMD(22, af_set_terminate_result), CMD(23, af_display_version),
    CMD(25, ret_u64_0), CMD(26, af_save_size), CMD(27, nop), CMD(28, af_save_size), CMD(30, nop), CMD(31, nop), CMD(32, nop), CMD(33, nop),
    CMD(40, ret_true), CMD(50, af_uuid), CMD(60, nop), CMD(65, ret_false), CMD(66, nop), CMD(67, nop), CMD(90, nop), CMD(100, nop),
    CMD(101, nop), CMD(102, nop), CMD(110, ret_u64_0), CMD(111, ret_u64_0), CMD(120, nop), CMD(121, nop), CMD(122, nop), CMD(123, ret_u32_0),
    CMD(130, af_gpu_error_event), CMD(140, af_misc_event), CMD(141, nop), CMD(150, af_misc_event), CMD(160, af_misc_event),
    CMD(210, af_misc_event), CMD(1001, nop)};
IFACE(g_af_iface, "nn::am::IApplicationFunctions", g_af_cmds);

/* ------------------------------------------------------------------ IApplicationProxy and appletOE */
static void ap_csg(IpcCtx *x) { out_obj(x, &g_csg_iface, 0); }
static void ap_sc(IpcCtx *x) { out_obj(x, &g_sc_iface, 0); }
static void ap_wc(IpcCtx *x) { out_obj(x, &g_wc_iface, 0); }
static void ap_ac(IpcCtx *x) { out_obj(x, &g_ac_iface, 0); }
static void ap_dc(IpcCtx *x) { out_obj(x, &g_dc_iface, 0); }
static void ap_lac(IpcCtx *x) { out_obj(x, &g_lac_iface, 0); }
static void ap_af(IpcCtx *x) { out_obj(x, &g_af_iface, 0); }
static void ap_dbg(IpcCtx *x) { out_obj(x, &g_dbg_iface, 0); }
CMD_TABLE(g_ap_cmds) = {CMD(0, ap_csg), CMD(1, ap_sc), CMD(2, ap_wc), CMD(3, ap_ac), CMD(4, ap_dc), CMD(11, ap_lac), CMD(20, ap_af), CMD(1000, ap_dbg)};
IFACE(g_ap_iface, "nn::am::IApplicationProxy", g_ap_cmds);

static void oe_open_proxy(IpcCtx *x) { out_obj(x, &g_ap_iface, 0); }
CMD_TABLE(g_oe_cmds) = {CMD(0, oe_open_proxy)};
IFACE(g_oe_iface, "nn::am::IApplicationProxyService", g_oe_cmds);
static const ServiceDef g_oe_def = {"appletOE", &g_oe_iface, 0};

/* ------------------------------------------------------------------ apm */
static void apm_session_set(IpcCtx *x) { (void)x; }
static void apm_session_get(IpcCtx *x) { out_u32(x, 0, 0x00020001); }   /* a plausible configuration id */
CMD_TABLE(g_apm_session_cmds) = {CMD(0, apm_session_set), CMD(1, apm_session_get), CMD(2, nop)};
IFACE(g_apm_session_iface, "nn::apm::ISession", g_apm_session_cmds);
static void apm_open_session(IpcCtx *x) { out_obj(x, &g_apm_session_iface, 0); }
CMD_TABLE(g_apm_cmds) = {CMD(0, apm_open_session), CMD(1, ret_u32_1), CMD(6, ret_false)};
IFACE(g_apm_iface, "nn::apm::IManager", g_apm_cmds);
static const ServiceDef g_apm_def = {"apm", &g_apm_iface, 0};
static const ServiceDef g_apm_am_def = {"apm:am", &g_apm_iface, 0};
CMD_TABLE(g_apm_sys_cmds) = {CMD(0, nop), CMD(6, nop), CMD(7, ret_u32_0)};
IFACE(g_apm_sys_iface, "nn::apm::ISystemManager", g_apm_sys_cmds);
static const ServiceDef g_apm_sys_def = {"apm:sys", &g_apm_sys_iface, 0};

void hle_applet_init(void) {
    hle_register_service(&g_oe_def);
    hle_register_service(&g_apm_def);
    hle_register_service(&g_apm_am_def);
    hle_register_service(&g_apm_sys_def);
}
