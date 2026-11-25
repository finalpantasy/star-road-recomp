/* Star Road Recomp system services the boot touches that need no real state (task-13): pctl (parental control), aoc:u (add-on
 * content), ectx:aw (error context). Command ids are interface facts (switchbrew "Services API"); behavior is that of a console
 * with no restrictions, no DLC and no pending errors. Our own code. */
#include "hle.h"

#define PROGRAM_ID 0x0100BC0018138000ULL

static void nop(IpcCtx *x) { (void)x; }
static void ret_true(IpcCtx *x) { out_bytes(x, 0, &(uint8_t){1}, 1); }
static void ret_false(IpcCtx *x) { out_bytes(x, 0, &(uint8_t){0}, 1); }
static void ret_u32_0(IpcCtx *x) { out_u32(x, 0, 0); }
static void ret_u64_0(IpcCtx *x) { out_u64(x, 0, 0); }

/* ------------------------------------------------------------------ pctl */
/* commands are added as the boot path shows them (every unknown command is logged by the IPC layer) */
CMD_TABLE(g_pctl_cmds) = {CMD(1, nop), CMD(1001, nop), CMD(1002, nop), CMD(1003, nop), CMD(1004, nop), CMD(1031, ret_false), CMD(1032, nop),
                          CMD(1061, nop), CMD(1062, nop), CMD(1063, nop), CMD(1064, ret_false), CMD(1403, ret_true), CMD(1406, ret_u32_0)};
IFACE(g_pctl_iface, "nn::pctl::IParentalControlService", g_pctl_cmds);
static void pctl_create(IpcCtx *x) { out_obj(x, &g_pctl_iface, 0); }
CMD_TABLE(g_pctl_factory_cmds) = {CMD(0, pctl_create), CMD(1, pctl_create)};
IFACE(g_pctl_factory_iface, "nn::pctl::IParentalControlServiceFactory", g_pctl_factory_cmds);
static const ServiceDef g_pctl_def[] = {{"pctl", &g_pctl_factory_iface, 0}, {"pctl:s", &g_pctl_factory_iface, 0}, {"pctl:r", &g_pctl_factory_iface, 0},
                                        {"pctl:a", &g_pctl_factory_iface, 0}};

/* ------------------------------------------------------------------ aoc:u (no add-on content) */
static KObj *g_aoc_event;
static void aoc_event(IpcCtx *x) {
    if (!g_aoc_event) g_aoc_event = k_event_named("aoc_list_changed", 0);
    out_copy_handle(x, k_handle_for(g_aoc_event, 1));
}
static void aoc_count(IpcCtx *x) { out_u32(x, 0, 0); }
static void aoc_list(IpcCtx *x) { out_u32(x, 0, 0); }
static void aoc_base_id(IpcCtx *x) { out_u64(x, 0, PROGRAM_ID + 0x1000); }
CMD_TABLE(g_aoc_cmds) = {CMD(0, aoc_count), CMD(1, aoc_list), CMD(2, aoc_count), CMD(3, aoc_list), CMD(4, aoc_base_id), CMD(5, aoc_base_id),
                         CMD(6, nop), CMD(7, nop), CMD(8, aoc_event), CMD(9, ret_u32_0), CMD(10, aoc_event)};
IFACE(g_aoc_iface, "nn::aocsrv::IAddOnContentManager", g_aoc_cmds);
static const ServiceDef g_aoc_def = {"aoc:u", &g_aoc_iface, 0};

/* ------------------------------------------------------------------ ectx:aw (error context registrar; the context is dropped) */
CMD_TABLE(g_ectx_cmds) = {CMD(0, nop), CMD(1, nop), CMD(2, nop), CMD(10, nop)};
IFACE(g_ectx_iface, "nn::err::context::IContextRegistrar", g_ectx_cmds);
static const ServiceDef g_ectx_def = {"ectx:aw", &g_ectx_iface, 0};

void hle_sys_init(void) {
    for (int i = 0; i < 4; i++) hle_register_service(&g_pctl_def[i]);
    hle_register_service(&g_aoc_def);
    hle_register_service(&g_ectx_def);
    (void)ret_u64_0;
}
