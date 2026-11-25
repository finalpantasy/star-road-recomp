/* Star Road Recomp HLE service layer: startup, paths, markers (task-13). Our own code. */
#include "hle.h"

uint64_t g_hle_applet_resource_user_id = 0x1;
volatile LONG g_level0_reached;

const char *hle_data_root(void) {
    static char p[512];
    if (!p[0]) {
        const char *e = getenv("SMRPG_WORK");
        snprintf(p, sizeof p, "%s", e && *e ? e : "A:/AI/Recomps/smrpg-work");
    }
    return p;
}
const char *hle_romfs_dir(void) {
    static char p[600];
    if (!p[0]) {
        const char *e = getenv("SMRPG_ROMFS");
        if (e && *e) snprintf(p, sizeof p, "%s", e);
        else snprintf(p, sizeof p, "%s/v101/romfs", hle_data_root());
    }
    return p;
}
const char *hle_save_dir(void) {
    static char p[600];
    if (!p[0]) {
        const char *e = getenv("SMRPG_SAVE");
        if (e && *e) snprintf(p, sizeof p, "%s", e);
        else snprintf(p, sizeof p, "%s/save", hle_data_root());
    }
    return p;
}

void hle_boot_marker(const char *what) {
    rt_log("========================================================================");
    rt_log("BOOT MARKER: %s", what);
    rt_log("========================================================================");
}

static volatile LONG g_init_done;
__attribute__((constructor)) static void hle_ctor(void) { hle_services_init(); }
void hle_services_init(void) {
    if (InterlockedCompareExchange(&g_init_done, 1, 0)) return;
    hle_sm_init();
    hle_fs_init();
    hle_applet_init();
    hle_misc_init();
    hle_hid_init();
    hle_audio_init();
    hle_time_init();
    hle_sys_init();
    hle_gfxsvc_init();
    hle_net_init();
}
