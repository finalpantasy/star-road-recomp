/* Star Road Recomp sm: service manager (task-13). Our own code.
 * Commands (switchbrew "Services API"): 0 RegisterClient(pid), 1 GetServiceHandle(name), 2 RegisterService, 3 UnregisterService, 4 DetachClient. */
#include "hle.h"

CMD_TABLE(g_dummy_cmds) = {{0xFFFFFFFFu, "none", 0}};
IFACE(g_dummy_iface, "<dummy>", g_dummy_cmds);

static void sm_register_client(IpcCtx *x) { (void)x; }

/* The service access list the process was granted (main.npdm, ACI0 service access control). sm refuses every other name, so the sdk's
 * own fallbacks run as on a console (for example vi:m and vi:s are refused to an application and nn::vi falls back to vi:u).
 * Read from the owner's extraction at run time; if it is missing, nothing is refused. */
#define SAC_MAX 96
static char g_sac[SAC_MAX][9];
static int g_nsac = -1;
static void sac_load(void) {
    g_nsac = 0;
    char path[640];
    snprintf(path, sizeof path, "%s/base/exefs/main.npdm", hle_data_root());
    FILE *f = fopen(path, "rb");
    if (!f) { rt_log("sm: %s not found, service access list not enforced", path); return; }
    static uint8_t d[0x1000];
    size_t n = fread(d, 1, sizeof d, f);
    fclose(f);
    if (n < 0x80 || memcmp(d, "META", 4)) return;
    uint32_t aci0 = *(uint32_t *)(d + 0x70);
    if (aci0 + 0x38 > n || memcmp(d + aci0, "ACI0", 4)) return;
    uint32_t so = *(uint32_t *)(d + aci0 + 0x28), ss = *(uint32_t *)(d + aci0 + 0x2C);
    for (uint32_t i = 0; i < ss && aci0 + so + i < n;) {
        uint8_t b = d[aci0 + so + i];
        uint32_t len = (b & 7) + 1u;
        if (g_nsac < SAC_MAX) { memcpy(g_sac[g_nsac], d + aci0 + so + i + 1, len); g_sac[g_nsac][len] = 0; g_nsac++; }
        i += 1 + len;
    }
    rt_log("sm: service access list loaded: %d names", g_nsac);
}
static int sac_allows(const char *name) {
    if (g_nsac < 0) sac_load();
    if (g_nsac == 0) return 1;
    for (int i = 0; i < g_nsac; i++) if (!strcmp(g_sac[i], name)) return 1;
    return 0;
}

static void sm_get_service(IpcCtx *x) {
    char name[9] = {0};
    memcpy(name, in_ptr(x, 0), 8);
    if (strcmp(name, "sm:") && !sac_allows(name)) {
        rt_log("sm: GetService(\"%s\") refused: not in the process's service access list", name);
        x->result = RES_SM_NOT_ALLOWED;
        return;
    }
    const ServiceDef *d = hle_find_service(name);
    if (!d) {
        char *nm = malloc(48);
        snprintf(nm, 48, "sm:GetService(%s)", name);
        rt_log("sm: GetService(\"%s\"): NOT REGISTERED (no HLE for this service)", name);
        hle_ipc_note_unknown(nm, 0);
        if (hle_permissive()) {
            SvcIface *di = calloc(1, sizeof *di);
            *di = g_dummy_iface;
            di->name = strdup(name);
            out_obj(x, di, 0);
            return;
        }
        x->result = RES_SM_NOT_ALLOWED;
        return;
    }
    rt_log("sm: GetService(\"%s\") -> %s", name, d->root->name);
    out_obj(x, d->root, d->make_state ? d->make_state() : 0);
}

static void sm_register_service(IpcCtx *x) {
    char name[9] = {0};
    memcpy(name, in_ptr(x, 0), 8);
    rt_log("sm: guest RegisterService(\"%s\") refused (the game does not host services)", name);
    x->result = RES_UNSUPPORTED;
}

static void sm_ok(IpcCtx *x) { (void)x; }

CMD_TABLE(g_sm_cmds) = {CMD(0, sm_register_client), CMD(1, sm_get_service), CMD(2, sm_register_service), CMD(3, sm_ok), CMD(4, sm_ok)};
IFACE(g_sm_iface, "sm:", g_sm_cmds);
static const ServiceDef g_sm_def = {"sm:", &g_sm_iface, 0};

void hle_sm_init(void) { hle_register_service(&g_sm_def); }
