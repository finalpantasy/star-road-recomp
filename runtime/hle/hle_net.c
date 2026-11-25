/* Star Road Recomp network services (task-13): nifm (network interface manager) and bsd (sockets). The console is offline: nifm reports
 * no connection and every socket call fails with ENETDOWN, so the sdk's nn::nifm and nn::socket initialize cleanly and the game's
 * online features see an unavailable network. Command ids are interface facts (switchbrew "Network Interface service" and "Sockets
 * service"). Our own code. */
#include "hle.h"

#define RES_NIFM_NO_CONNECTION MAKE_RES(110, 300)
#define BSD_ENETDOWN 50u

static void nop(IpcCtx *x) { (void)x; }
static void ret_false(IpcCtx *x) { out_bytes(x, 0, &(uint8_t){0}, 1); }
static void ret_u32_0(IpcCtx *x) { out_u32(x, 0, 0); }

/* ------------------------------------------------------------------ nifm */
static void req_state(IpcCtx *x) { out_u32(x, 0, 1); }                 /* RequestState_Free: nothing submitted */
static void req_result(IpcCtx *x) { x->result = RES_NIFM_NO_CONNECTION; }
static KObj *g_nifm_ev[2];
static void req_events(IpcCtx *x) {
    for (int i = 0; i < 2; i++) {
        if (!g_nifm_ev[i]) g_nifm_ev[i] = k_event_named(i ? "nifm_req_event1" : "nifm_req_event0", 0);
        out_copy_handle(x, k_handle_for(g_nifm_ev[i], 1));
    }
}
CMD_TABLE(g_nreq_cmds) = {CMD(0, req_state), CMD(1, req_result), CMD(2, req_events), CMD(3, nop), CMD(4, nop), CMD(5, nop), CMD(6, nop), CMD(8, nop),
                          CMD(9, nop), CMD(10, nop), CMD(11, nop), CMD(12, nop), CMD(13, nop), CMD(14, nop), CMD(15, nop)};
IFACE(g_nreq_iface, "nn::nifm::detail::IRequest", g_nreq_cmds);
static void gs_create_request(IpcCtx *x) { out_obj(x, &g_nreq_iface, 0); }
static void gs_client_id(IpcCtx *x) { out_u64(x, 0, 1); }
static void gs_no_connection(IpcCtx *x) { x->result = RES_NIFM_NO_CONNECTION; }
static void gs_status(IpcCtx *x) { out_zero(x, 0, 3); }               /* not connected */
CMD_TABLE(g_ngs_cmds) = {CMD(1, gs_client_id), CMD(4, gs_create_request), CMD(5, gs_no_connection), CMD(12, gs_no_connection), CMD(15, gs_no_connection),
                         CMD(18, gs_status), CMD(21, ret_false), CMD(22, ret_false), CMD(23, nop), CMD(24, nop), CMD(25, nop), CMD(26, ret_false),
                         CMD(27, nop), CMD(28, nop), CMD(29, nop), CMD(30, ret_u32_0)};
IFACE(g_ngs_iface, "nn::nifm::detail::IGeneralService", g_ngs_cmds);
static void nifm_create_general(IpcCtx *x) { out_obj(x, &g_ngs_iface, 0); }
CMD_TABLE(g_nifm_cmds) = {CMD(4, nifm_create_general), CMD(5, nifm_create_general)};
IFACE(g_nifm_iface, "nn::nifm::detail::IStaticService", g_nifm_cmds);
static const ServiceDef g_nifm_def[] = {{"nifm:u", &g_nifm_iface, 0}, {"nifm:a", &g_nifm_iface, 0}, {"nifm:s", &g_nifm_iface, 0}};

/* ------------------------------------------------------------------ bsd */
static void bsd_register(IpcCtx *x) { out_u64(x, 0, 0); }               /* RegisterClient answers the client's process id (u64); the transfer memory stays unused */
static void bsd_fail(IpcCtx *x) { out_u32(x, 0, (uint32_t)-1); out_u32(x, 4, BSD_ENETDOWN); }
static void bsd_ok(IpcCtx *x) { out_u32(x, 0, 0); out_u32(x, 4, 0); }
CMD_TABLE(g_bsd_cmds) = {CMD(0, bsd_register), CMD(1, nop), CMD(2, bsd_fail), CMD(3, bsd_fail), CMD(4, bsd_fail), CMD(5, bsd_fail), CMD(6, bsd_fail),
                         CMD(7, bsd_fail), CMD(8, bsd_fail), CMD(9, bsd_fail), CMD(10, bsd_fail), CMD(11, bsd_fail), CMD(12, bsd_fail), CMD(13, bsd_fail),
                         CMD(14, bsd_fail), CMD(15, bsd_fail), CMD(16, bsd_fail), CMD(17, bsd_fail), CMD(18, bsd_fail), CMD(19, bsd_fail), CMD(20, bsd_fail),
                         CMD(21, bsd_fail), CMD(22, bsd_fail), CMD(23, bsd_ok), CMD(24, bsd_fail), CMD(25, bsd_fail), CMD(26, bsd_ok), CMD(27, bsd_fail),
                         CMD(29, bsd_fail), CMD(30, bsd_fail)};
IFACE(g_bsd_iface, "nn::socket::sf::IClient", g_bsd_cmds);
static const ServiceDef g_bsd_def[] = {{"bsd:u", &g_bsd_iface, 0}, {"bsd:s", &g_bsd_iface, 0}, {"bsd:a", &g_bsd_iface, 0}};

/* ------------------------------------------------------------------ ssl (ISslService): the game only sets the interface version */
CMD_TABLE(g_ssl_cmds) = {CMD(5, nop)};
IFACE(g_ssl_iface, "nn::ssl::sf::ISslService", g_ssl_cmds);
static const ServiceDef g_ssl_def = {"ssl", &g_ssl_iface, 0};

void hle_net_init(void) {
    hle_register_service(&g_ssl_def);
    for (int i = 0; i < 3; i++) hle_register_service(&g_nifm_def[i]);
    for (int i = 0; i < 3; i++) hle_register_service(&g_bsd_def[i]);
}
