/* Star Road Recomp small Horizon services (task-13): lm (logging), plus the stubs the boot needs. Our own code.
 * Command ids are interface facts (switchbrew "Services API"); nothing is copied from another emulator. */
#include "hle.h"

/* ------------------------------------------------------------------ lm (ILogService / ILogger) */
/* Log packet: header 0x18 (u64 pid, u64 tid, u32 flags, u8 pad, u8 severity, u8 verbosity, u8 payload size), then chunks {u8 type, u8 len, data};
 * chunk 2 = message text. Long messages arrive in several packets (flags bit 0 head, bit 1 tail). */
static void lm_log(IpcCtx *x) {
    /* the packet travels in a pointer (X) or a send (A) buffer depending on its size; the other descriptor is present but empty */
    IpcBuf *b = ipc_in_buf(x, 0);
    if ((!b || b->size < 0x18) && x->npa + x->npx > 1) b = ipc_in_buf(x, 1);
    if (!b || b->size < 0x18) { rt_log("guest lm: short or missing buffer (%llu bytes)", b ? (unsigned long long)b->size : 0ull); return; }
    const uint8_t *p = (const uint8_t *)b->addr;
    uint32_t flags = *(const uint32_t *)(p + 0x10);
    uint32_t psize = *(const uint32_t *)(p + 0x14) >> 24;
    (void)psize;
    static __thread char acc[2048];
    static __thread size_t nacc;
    uint64_t off = 0x18;
    while (off + 2 <= b->size) {
        uint8_t t = p[off] & 0x7F, len = p[off + 1];
        off += 2;
        if (off + len > b->size) break;
        if (t == 2) {
            size_t n = len;
            if (nacc + n >= sizeof acc) n = sizeof acc - 1 - nacc;
            memcpy(acc + nacc, p + off, n);
            nacc += n;
        }
        off += len;
    }
    if (flags & 2) {
        acc[nacc] = 0;
        for (size_t i = 0; i < nacc; i++) if (acc[i] == '\n' || acc[i] == '\r') acc[i] = ' ';
        rt_log("guest lm: %s", acc);
        nacc = 0;
    }
}
static void lm_set_destination(IpcCtx *x) { (void)x; }
CMD_TABLE(g_logger_cmds) = {CMD(0, lm_log), CMD(1, lm_set_destination)};
IFACE(g_logger_iface, "nn::lm::ILogger", g_logger_cmds);
static void lm_open_logger(IpcCtx *x) { out_obj(x, &g_logger_iface, 0); }
CMD_TABLE(g_lm_cmds) = {CMD(0, lm_open_logger)};
IFACE(g_lm_iface, "nn::lm::ILogService", g_lm_cmds);
static const ServiceDef g_lm_def = {"lm", &g_lm_iface, 0};

/* ------------------------------------------------------------------ ldr:ro (IRoInterface)
 * nn::ro::Initialize registers the process with the loader service. The game loads no NRO modules, so the module commands only
 * need to answer politely: 0 MapManualLoadModuleMemory, 1 Unmap, 2 RegisterModuleInfo, 3 UnregisterModuleInfo, 4 RegisterProcessHandle,
 * 10 RegisterProcessModuleInfo. */
static void ro_ok(IpcCtx *x) { (void)x; }
static void ro_map(IpcCtx *x) { out_u64(x, 0, 0); }
CMD_TABLE(g_ro_cmds) = {CMD(0, ro_map), CMD(1, ro_ok), CMD(2, ro_ok), CMD(3, ro_ok), CMD(4, ro_ok), CMD(10, ro_ok)};
IFACE(g_ro_iface, "nn::ro::detail::IRoInterface", g_ro_cmds);
static const ServiceDef g_ro_def = {"ldr:ro", &g_ro_iface, 0};

void hle_misc_init(void) {
    hle_register_service(&g_lm_def);
    hle_register_service(&g_ro_def);
}
