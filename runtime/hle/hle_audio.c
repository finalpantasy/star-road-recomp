/* Star Road Recomp audio services (task-13): discovery stubs. Our own code.
 * The boot up to the first read of Data/level0 never opens audren:u, audout:u, hwopus or acc:u0 (disc76 log: 0 requests), so these four are
 * registered with no commands yet. The first command the guest sends is logged as an unknown command (the guest then gets an error), which is
 * how the real command list gets discovered. The null sink (no Windows audio unless --audio) goes in here when audren is first needed. */
#include "hle.h"

static void audio_none(IpcCtx *x) { (void)x; }
CMD_TABLE(g_audio_cmds) = {CMD(0xFFFF, audio_none)};   /* placeholder: a table needs one entry; no guest sends id 0xFFFF */
IFACE(g_audren_iface, "nn::audio::detail::IAudioRendererManager", g_audio_cmds);
IFACE(g_audout_iface, "nn::audio::detail::IAudioOutManager", g_audio_cmds);
IFACE(g_hwopus_iface, "nn::codec::detail::IHardwareOpusDecoderManager", g_audio_cmds);
IFACE(g_acc_iface, "nn::account::detail::IAccountServiceForApplication", g_audio_cmds);
static const ServiceDef g_audio_defs[] = {{"audren:u", &g_audren_iface, 0}, {"audout:u", &g_audout_iface, 0}, {"hwopus", &g_hwopus_iface, 0},
                                          {"acc:u0", &g_acc_iface, 0}};

void hle_audio_init(void) {
    for (size_t i = 0; i < sizeof g_audio_defs / sizeof g_audio_defs[0]; i++) hle_register_service(&g_audio_defs[i]);
}
