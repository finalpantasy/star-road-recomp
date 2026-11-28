/* Star Road Recomp time services (task-13): time:u, time:a, time:s, time:r. Our own code.
 * The sdk reads its clocks from a shared memory block (steady, local and network clock contexts, each a u32 update counter and two
 * copies of the context) plus the system tick, and asks the service for calendar conversion with the TimeZoneRule it loaded.
 * We keep the device on UTC: the shared memory is filled from the host clock once, calendar conversion is done here.
 * Layouts: switchbrew "Services API: Time". */
#include "hle.h"
#include <time.h>

static const uint8_t g_steady_uuid[16] = {0x53, 0x52, 0x52, 0x43, 0x2D, 0x53, 0x54, 0x45, 0x41, 0x44, 0x59, 0x2D, 0x43, 0x4C, 0x4B, 0x31};
static KObj *g_time_shm;

static int64_t steady_secs(void) { return (int64_t)(rt_cntvct() / RT_CNTFRQ); }
static int64_t posix_now(void) { return rt_deterministic() ? 1767225600LL + steady_secs() - 100000 : (int64_t)time(0); }  /* deterministic: 2026-01-01 UTC plus virtual seconds */

static void shm_put(uint8_t *base, uint32_t off, uint32_t size, const void *ctx) {
    uint32_t cnt = 2;
    memcpy(base + off, &cnt, 4);
    memcpy(base + off + 8, ctx, size);
    memcpy(base + off + 8 + size, ctx, size);
}
static KObj *time_shm(void) {
    if (g_time_shm) return g_time_shm;
    g_time_shm = k_shmem_new(0x1000);
    uint8_t *b = (uint8_t *)k_shmem_ptr(g_time_shm);
    memset(b, 0, 0x1000);
    uint8_t steady[0x18] = {0};
    memcpy(steady + 8, g_steady_uuid, 16);                 /* internal offset 0, clock source id */
    shm_put(b, 0x00, 0x18, steady);
    uint8_t ctx[0x20] = {0};
    int64_t off = posix_now() - steady_secs(), tp = steady_secs();
    memcpy(ctx, &off, 8);
    memcpy(ctx + 8, &tp, 8);
    memcpy(ctx + 16, g_steady_uuid, 16);
    shm_put(b, 0x38, 0x20, ctx);                           /* local system clock */
    shm_put(b, 0x80, 0x20, ctx);                           /* network system clock */
    b[0xC8] = 0;                                           /* automatic correction disabled */
    return g_time_shm;
}

/* ---- ISystemClock */
static void clk_current_time(IpcCtx *x) { out_u64(x, 0, (uint64_t)posix_now()); }
static void clk_context(IpcCtx *x) {
    uint8_t ctx[0x20] = {0};
    int64_t off = posix_now() - steady_secs(), tp = steady_secs();
    memcpy(ctx, &off, 8);
    memcpy(ctx + 8, &tp, 8);
    memcpy(ctx + 16, g_steady_uuid, 16);
    out_bytes(x, 0, ctx, 0x20);
}
static void nop(IpcCtx *x) { (void)x; }
static void clk_event(IpcCtx *x) { out_copy_handle(x, k_handle_for(k_event_new(0), 1)); }
CMD_TABLE(g_sysclk_cmds) = {CMD(0, clk_current_time), CMD(1, nop), CMD(2, clk_context), CMD(3, nop), CMD(4, clk_event)};
IFACE(g_sysclk_iface, "nn::timesrv::ISystemClock", g_sysclk_cmds);

/* ---- ISteadyClock */
static void st_time_point(IpcCtx *x) {
    uint8_t tp[0x18] = {0};
    int64_t v = steady_secs();
    memcpy(tp, &v, 8);
    memcpy(tp + 8, g_steady_uuid, 16);
    out_bytes(x, 0, tp, 0x18);
}
static void st_zero64(IpcCtx *x) { out_u64(x, 0, 0); }
static void st_false(IpcCtx *x) { out_bytes(x, 0, &(uint8_t){0}, 1); }
static void st_zero32(IpcCtx *x) { out_u32(x, 0, 0); }
CMD_TABLE(g_steady_cmds) = {CMD(0, st_time_point), CMD(2, st_zero64), CMD(3, nop), CMD(100, st_zero64), CMD(101, st_false), CMD(102, st_zero32),
                            CMD(200, st_zero64), CMD(201, nop)};
IFACE(g_steady_iface, "nn::timesrv::ISteadyClock", g_steady_cmds);

/* ---- ITimeZoneService (UTC only; the rule buffer is opaque to us) */
static void tz_name(IpcCtx *x) { char n[0x24] = "UTC"; out_bytes(x, 0, n, 0x24); }
static void tz_count(IpcCtx *x) { out_u32(x, 0, 1); }
static void tz_load_names(IpcCtx *x) {
    char n[0x24] = "UTC";
    IpcBuf *b = ipc_out_buf(x, 0);
    uint32_t cnt = 0;
    if (b && b->size >= 0x24) { memcpy((void *)b->addr, n, 0x24); cnt = 1; }
    out_u32(x, 0, cnt);
}
static void tz_load_rule(IpcCtx *x) {
    IpcBuf *b = ipc_out_buf(x, 0);
    if (b) memset((void *)b->addr, 0, b->size);              /* an all-zero rule: no transitions, zero offset, UTC */
}
static void tz_rule_version(IpcCtx *x) { char v[16] = "2022a"; out_bytes(x, 0, v, 16); }
static void tz_name_and_time(IpcCtx *x) {
    char n[0x24] = "UTC";
    out_bytes(x, 0, n, 0x24);
    uint8_t tp[0x18] = {0};
    memcpy(tp + 8, g_steady_uuid, 16);
    out_bytes(x, 0x28, tp, 0x18);
}
/* days from civil (Howard Hinnant's public domain algorithm) */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y += *m <= 2;
}
static void to_calendar(IpcCtx *x) {
    int64_t t = (int64_t)in_u64(x, 0);
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t rem = t - days * 86400;
    int64_t y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    int16_t year = (int16_t)y;
    uint8_t cal[8] = {0};
    memcpy(cal, &year, 2);
    cal[2] = (uint8_t)m; cal[3] = (uint8_t)d; cal[4] = (uint8_t)(rem / 3600); cal[5] = (uint8_t)((rem % 3600) / 60); cal[6] = (uint8_t)(rem % 60);
    out_bytes(x, 0, cal, 8);
    uint32_t dow = (uint32_t)(((days % 7) + 11) % 7);       /* 1970-01-01 was a Thursday (4) */
    uint32_t doy = (uint32_t)(days - days_from_civil(y, 1, 1));
    uint8_t add[0x18] = {0};
    memcpy(add, &dow, 4); memcpy(add + 4, &doy, 4);
    memcpy(add + 8, "UTC", 4);
    out_bytes(x, 8, add, 0x18);
}
static void to_posix(IpcCtx *x) {
    const uint8_t *c = in_ptr(x, 0);
    int16_t year; memcpy(&year, c, 2);
    int64_t days = days_from_civil(year, c[2], c[3]);
    int64_t t = days * 86400 + c[4] * 3600 + c[5] * 60 + c[6];
    ipc_write_out(x, 0, &t, 8);
    out_u32(x, 0, 1);
}
CMD_TABLE(g_tz_cmds) = {CMD(0, tz_name), CMD(1, nop), CMD(2, tz_count), CMD(3, tz_load_names), CMD(4, tz_load_rule), CMD(5, tz_rule_version),
                        CMD(6, tz_name_and_time), CMD(20, clk_event), CMD(100, to_calendar), CMD(101, to_calendar), CMD(201, to_posix),
                        CMD(202, to_posix)};
IFACE(g_tz_iface, "nn::timesrv::ITimeZoneService", g_tz_cmds);

/* ---- IStaticService */
static void ss_user(IpcCtx *x) { out_obj(x, &g_sysclk_iface, 0); }
static void ss_steady(IpcCtx *x) { out_obj(x, &g_steady_iface, 0); }
static void ss_tz(IpcCtx *x) { out_obj(x, &g_tz_iface, 0); }
static void ss_shm(IpcCtx *x) { out_copy_handle(x, k_handle_for(time_shm(), 0)); }
static void ss_true(IpcCtx *x) { out_bytes(x, 0, &(uint8_t){1}, 1); }
static void ss_false(IpcCtx *x) { out_bytes(x, 0, &(uint8_t){0}, 1); }
static void ss_year(IpcCtx *x) { out_u32(x, 0, 2000); }
/* ClockSnapshot (0xD0): user/network contexts, user/network time, calendars and infos, steady time point, location name */
static void ss_snapshot(IpcCtx *x) {
    uint8_t s[0xD0] = {0};
    int64_t now = posix_now(), st = steady_secs(), off = now - st;
    memcpy(s, &off, 8); memcpy(s + 8, &st, 8); memcpy(s + 16, g_steady_uuid, 16);
    memcpy(s + 0x20, s, 0x20);
    memcpy(s + 0x40, &now, 8); memcpy(s + 0x48, &now, 8);
    IpcCtx t = {0};
    memcpy(t.out, &now, 8);
    t.in = (const uint8_t *)&now; t.in_size = 8;
    to_calendar(&t);
    memcpy(s + 0x50, t.out, 8); memcpy(s + 0x58, t.out, 8);
    memcpy(s + 0x60, t.out + 8, 0x18); memcpy(s + 0x78, t.out + 8, 0x18);
    memcpy(s + 0x90, &st, 8); memcpy(s + 0x98, g_steady_uuid, 16);
    memcpy(s + 0xA8, "UTC", 4);
    IpcBuf *b = ipc_out_buf(x, 0);
    if (b && b->size >= 0xD0) ipc_write_out(x, 0, s, 0xD0);
}
static void ss_span(IpcCtx *x) { out_u64(x, 0, 0); }
static void ss_calc_base(IpcCtx *x) { out_u64(x, 0, 0); }
CMD_TABLE(g_static_cmds) = {
    CMD(0, ss_user), CMD(1, ss_user), CMD(2, ss_steady), CMD(3, ss_tz), CMD(4, ss_user), CMD(5, ss_user), CMD(20, ss_shm), CMD(50, nop),
    CMD(51, st_zero64), CMD(100, ss_false), CMD(101, nop), CMD(102, ss_year), CMD(200, ss_true), CMD(201, st_zero64), CMD(300, ss_calc_base),
    CMD(400, ss_snapshot), CMD(401, ss_snapshot), CMD(500, ss_span), CMD(501, ss_span)};
IFACE(g_static_iface, "nn::timesrv::IStaticService", g_static_cmds);
static const ServiceDef g_time_u = {"time:u", &g_static_iface, 0}, g_time_a = {"time:a", &g_static_iface, 0}, g_time_s = {"time:s", &g_static_iface, 0},
                        g_time_r = {"time:r", &g_static_iface, 0};

void hle_time_init(void) {
    hle_register_service(&g_time_u);
    hle_register_service(&g_time_a);
    hle_register_service(&g_time_s);
    hle_register_service(&g_time_r);
}
