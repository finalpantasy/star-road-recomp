/* Star Road Recomp input service (task-13): hid. The game reads pad state from the shared memory block the service hands out; the
 * block stays zero (no controller connected) until the input task fills it. Commands answer with success and neutral values so the
 * sdk's nn::hid initialization runs through. Command ids are interface facts (switchbrew "HID services"). Our own code. */
#include "hle.h"

static void nop(IpcCtx *x) { (void)x; }
static void ret_u32_0(IpcCtx *x) { out_u32(x, 0, 0); }
static void ret_u64_0(IpcCtx *x) { out_u64(x, 0, 0); }
static void ret_u8_0(IpcCtx *x) { uint8_t b = 0; out_bytes(x, 0, &b, 1); }

/* IAppletResource */
#define HID_SHMEM_SIZE 0x40000
static KObj *g_hid_shmem;
static void ar_shmem(IpcCtx *x) {
    if (!g_hid_shmem) g_hid_shmem = k_shmem_new(HID_SHMEM_SIZE);
    out_copy_handle(x, k_handle_for(g_hid_shmem, 0));
}
CMD_TABLE(g_ares_cmds) = {CMD(0, ar_shmem)};
IFACE(g_ares_iface, "nn::hid::IAppletResource", g_ares_cmds);

/* IActiveVibrationDeviceList */
CMD_TABLE(g_avdl_cmds) = {CMD(0, nop)};
IFACE(g_avdl_iface, "nn::hid::IActiveVibrationDeviceList", g_avdl_cmds);

static void hid_create_applet_resource(IpcCtx *x) { out_obj(x, &g_ares_iface, 0); }
static void hid_create_vibration_list(IpcCtx *x) { out_obj(x, &g_avdl_iface, 0); }
static KObj *g_hid_ev[4];
static void hid_event_handle(IpcCtx *x) {
    int i = (int)(x->cmd & 3);
    if (!g_hid_ev[i]) g_hid_ev[i] = k_event_named("hid_event", 0);
    out_copy_handle(x, k_handle_for(g_hid_ev[i], 1));
}
static void hid_vib_info(IpcCtx *x) { out_zero(x, 0, 8); }

CMD_TABLE(g_hid_cmds) = {
    CMD(0, hid_create_applet_resource), CMD(1, nop), CMD(11, nop), CMD(21, nop), CMD(31, nop), CMD(32, nop), CMD(40, hid_event_handle),
    CMD(41, nop), CMD(55, ret_u64_0), CMD(66, nop), CMD(67, nop), CMD(91, nop), CMD(100, nop), CMD(101, ret_u32_0), CMD(102, nop), CMD(103, nop),
    CMD(104, nop), CMD(106, hid_event_handle), CMD(108, ret_u64_0), CMD(109, nop), CMD(120, nop), CMD(121, ret_u64_0), CMD(122, nop),
    CMD(123, nop), CMD(124, nop), CMD(125, nop), CMD(126, nop), CMD(127, nop), CMD(128, nop), CMD(129, ret_u64_0), CMD(130, nop),
    CMD(131, nop), CMD(132, nop), CMD(200, hid_vib_info), CMD(201, nop), CMD(203, hid_create_vibration_list), CMD(204, nop),
    CMD(205, ret_u8_0), CMD(206, nop), CMD(300, nop), CMD(301, nop), CMD(302, nop), CMD(303, nop), CMD(304, nop), CMD(305, nop),
    CMD(306, nop), CMD(307, nop), CMD(308, nop)};
IFACE(g_hid_iface, "nn::hid::IHidServer", g_hid_cmds);
static const ServiceDef g_hid_def = {"hid", &g_hid_iface, 0};

void hle_hid_init(void) { hle_register_service(&g_hid_def); }
