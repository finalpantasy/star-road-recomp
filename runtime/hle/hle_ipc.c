/* Star Road Recomp HIPC/CMIF server side (task-13). Our own code.
 *
 * Request: the message sits at the start of the thread local region (TPIDRRO_EL0). Layout (libnx names):
 *   u32 w0: type[15:0] nX[19:16] nA[23:20] nB[27:24] nW[31:28]
 *   u32 w1: raw words[9:0] recv-list mode[13:10] recv-list offset[30:20] special[31]
 *   special header (u32: pid[0] ncopy[4:1] nmove[8:5]), optional u64 pid, copy handles, move handles
 *   X descriptors (8 bytes), A/B/W descriptors (12 bytes), raw data (16 byte aligned), C descriptors (8 bytes)
 * Raw data of a CMIF request: [domain header 16][SFCI magic, version, command, token][payload][domain object ids].
 * Response: same framing with type 0, raw data [domain out header 16][SFCO, 0, result, 0][payload][out object ids]. */
#include "hle.h"

#define TYPE_CLOSE 2
#define TYPE_REQUEST 4
#define TYPE_CONTROL 5
#define TYPE_REQUEST_CTX 6
#define TYPE_CONTROL_CTX 7

struct Session {
    char name[24];
    SvcObj *root;
    int domain;
    SvcObj **objs;      /* domain objects, id = index + 1; objs[0] is the root */
    int nobjs, capobjs;
    struct Session *dom; /* a session cloned from a domain session shares that session's object table (the kernel shares the domain) */
    volatile LONG refs;  /* the session itself plus the clones that point at it */
};
static struct Session *D(struct Session *s) { return s->dom ? s->dom : s; }

static CRITICAL_SECTION g_ipc;
static volatile LONG g_ipc_init;
static void ipc_lock_init(void) {
    if (InterlockedCompareExchange(&g_ipc_init, 1, 0) == 0) { InitializeCriticalSection(&g_ipc); g_ipc_init = 2; }
    while (g_ipc_init != 2) Sleep(0);
}

/* ------------------------------------------------------------------ objects */
SvcObj *svcobj_new(const SvcIface *iface, void *state) {
    SvcObj *o = calloc(1, sizeof *o);
    o->iface = iface; o->state = state; o->refs = 1;
    return o;
}

void svcobj_release(SvcObj *o) {
    if (!o) return;
    if (InterlockedDecrement(&o->refs) == 0) {
        if (o->iface && o->iface->destroy) o->iface->destroy(o->state);
        free(o);
    }
}

static Session *session_new(const char *name, SvcObj *root) {
    Session *s = calloc(1, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->root = root;
    s->refs = 1;
    return s;
}

void hle_session_closed(void *user) {
    Session *s = user;
    if (!s) return;
    if (s->dom) { Session *d = s->dom; free(s); hle_session_closed(d); return; }
    if (InterlockedDecrement(&s->refs) > 0) return;
    if (s->domain) {
        for (int i = 0; i < s->nobjs; i++) svcobj_release(s->objs[i]);
        free(s->objs);
    } else {
        svcobj_release(s->root);
    }
    free(s);
}

/* i-th interface object passed as an argument: a domain object id inside a domain, else a moved session handle */
SvcObj *ipc_in_obj(IpcCtx *x, int i) {
    if (x->is_domain) {
        Session *d = D(x->sess);
        if (i < 0 || i >= x->nin_objs) return 0;
        uint32_t id = x->in_objs[i];
        return id && id <= (uint32_t)d->nobjs ? d->objs[id - 1] : 0;
    }
    if (i < 0 || i >= x->nmove) return 0;
    Session *s = k_session_user(x->move_h[i]);
    return s ? s->root : 0;
}

static int domain_add(Session *s, SvcObj *o) {
    s = D(s);
    if (s->nobjs == s->capobjs) {
        s->capobjs = s->capobjs ? s->capobjs * 2 : 16;
        s->objs = realloc(s->objs, (size_t)s->capobjs * sizeof *s->objs);
    }
    s->objs[s->nobjs++] = o;
    return s->nobjs;       /* object id */
}

/* ------------------------------------------------------------------ service registry */
static const ServiceDef *g_services[128];
static int g_nservices;
void hle_register_service(const ServiceDef *d) { if (g_nservices < 128) g_services[g_nservices++] = d; }
const ServiceDef *hle_find_service(const char *name) {
    for (int i = 0; i < g_nservices; i++) if (!strcmp(g_services[i]->name, name)) return g_services[i];
    return 0;
}

/* ------------------------------------------------------------------ coverage */
typedef struct { const char *iface; uint32_t cmd; const char *name; LONG64 count; int unknown; } Cov;
static Cov g_cov[2048];
static int g_ncov;
static volatile LONG g_nunknown;
static int g_permissive = -1;     /* SMRPG_HLE_PERMISSIVE=1: unknown services and commands answer success with zeroes (discovery runs only) */
static int permissive(void) {
    if (g_permissive < 0) { const char *e = getenv("SMRPG_HLE_PERMISSIVE"); g_permissive = e && *e == '1'; }
    return g_permissive;
}
int hle_permissive(void) { return permissive(); }

static Cov *cov_get(const char *iface, uint32_t cmd, const char *name, int unknown) {
    for (int i = 0; i < g_ncov; i++)
        if (g_cov[i].cmd == cmd && !strcmp(g_cov[i].iface, iface)) return &g_cov[i];
    if (g_ncov >= 2048) return &g_cov[2047];
    g_cov[g_ncov] = (Cov){iface, cmd, name, 0, unknown};
    return &g_cov[g_ncov++];
}

void hle_ipc_note_unknown(const char *iface, uint32_t cmd) {
    Cov *c = cov_get(iface, cmd, "?", 1);
    if (++c->count == 1) {
        InterlockedIncrement(&g_nunknown);
        rt_log("IPC UNKNOWN COMMAND: interface %s cmd %u", iface, cmd);
    }
}

void hle_ipc_summary(void) {
    int nk = 0;
    rt_log("---- IPC coverage (interface, cmd, name, count) ----");
    for (int i = 0; i < g_ncov; i++) {
        rt_log("  %-34s %5u %-34s x%lld%s", g_cov[i].iface, g_cov[i].cmd, g_cov[i].name, (long long)g_cov[i].count, g_cov[i].unknown ? "  UNKNOWN" : "");
        if (g_cov[i].unknown) nk++;
    }
    rt_log("IPC: %d distinct commands used, %d unknown", g_ncov, nk);
}

/* ------------------------------------------------------------------ helpers for handlers */
void out_copy_handle(IpcCtx *x, uint32_t h) { if (x->r_ncopy < 8) x->r_copy[x->r_ncopy++] = h; }
void out_move_handle(IpcCtx *x, uint32_t h) { if (x->r_nmove < 8) x->r_move[x->r_nmove++] = h; }
void out_obj(IpcCtx *x, const SvcIface *iface, void *state) {
    if (x->r_nobj < 8) { x->r_obj[x->r_nobj].iface = iface; x->r_obj[x->r_nobj].state = state; x->r_nobj++; }
}

IpcBuf *ipc_in_buf(IpcCtx *x, int i) {
    if (i < x->npa) return &x->pa[i];
    i -= x->npa;
    if (i < x->npx) return &x->px[i];
    return 0;
}
IpcBuf *ipc_out_buf(IpcCtx *x, int i) {
    if (i < x->npb) return &x->pb[i];
    i -= x->npb;
    if (i < x->npc) return &x->pc[i];
    return 0;
}
/* An auto-select (HipcAutoSelect) buffer arrives as both a map-alias and a pointer descriptor and only one carries bytes: these find it. */
int ipc_in_data_idx(IpcCtx *x) { int n = x->npa + x->npx; for (int i = 0; i < n; i++) { IpcBuf *b = ipc_in_buf(x, i); if (b && b->size) return i; } return 0; }
int ipc_out_data_idx(IpcCtx *x) { int n = x->npb + x->npc; for (int i = 0; i < n; i++) { IpcBuf *b = ipc_out_buf(x, i); if (b && b->size) return i; } return 0; }
uint64_t ipc_write_out(IpcCtx *x, int i, const void *data, uint64_t n) {
    IpcBuf *b = ipc_out_buf(x, i);
    if (!b) return 0;
    uint64_t w = n < b->size ? n : b->size;
    memcpy((void *)b->addr, data, w);
    if (i >= x->npb && x->r_nx < IPC_MAXBUF) {
        IpcBuf r = {b->addr, w, b->idx};
        x->r_x[x->r_nx++] = r;
    }
    return w;
}

/* ------------------------------------------------------------------ parse */
static int ipc_parse(uint8_t *tls, IpcCtx *x, uint32_t *domain_cmd, uint32_t *domain_obj, uint32_t *domain_data) {
    uint32_t *m = (uint32_t *)tls;
    uint32_t w0 = m[0], w1 = m[1];
    x->type = w0 & 0xFFFF;
    int nx = (w0 >> 16) & 0xF, na = (w0 >> 20) & 0xF, nb = (w0 >> 24) & 0xF, nw = (w0 >> 28) & 0xF;
    uint32_t words = w1 & 0x3FF, rmode = (w1 >> 10) & 0xF, roff = (w1 >> 20) & 0x7FF, special = w1 >> 31;
    uint32_t *p = m + 2;
    if (special) {
        uint32_t sp = *p++;
        x->has_pid = sp & 1;
        int ncopy = (sp >> 1) & 0xF, nmove = (sp >> 5) & 0xF;
        if (x->has_pid) { memcpy(&x->pid, p, 8); p += 2; }
        for (int i = 0; i < ncopy; i++) { uint32_t h = *p++; if (x->ncopy < 16) x->copy_h[x->ncopy++] = h; }
        for (int i = 0; i < nmove; i++) { uint32_t h = *p++; if (x->nmove < 16) x->move_h[x->nmove++] = h; }
    }
    for (int i = 0; i < nx; i++, p += 2) {
        uint32_t d0 = p[0], d1 = p[1];
        IpcBuf b;
        b.idx = d0 & 0x3F;
        b.addr = d1 | ((uint64_t)((d0 >> 12) & 0xF) << 32) | ((uint64_t)((d0 >> 6) & 0x3F) << 36);
        b.size = d0 >> 16;
        if (x->npx < IPC_MAXBUF) x->px[x->npx++] = b;
    }
    for (int k = 0; k < 3; k++) {
        int n = k == 0 ? na : k == 1 ? nb : nw;
        IpcBuf *arr = k == 0 ? x->pa : k == 1 ? x->pb : x->pw;
        int *cnt = k == 0 ? &x->npa : k == 1 ? &x->npb : &x->npw;
        for (int i = 0; i < n; i++, p += 3) {
            uint32_t w = p[2];
            IpcBuf b;
            b.idx = (uint32_t)i;
            b.size = p[0] | ((uint64_t)((w >> 24) & 0xF) << 32);
            b.addr = p[1] | ((uint64_t)((w >> 28) & 0xF) << 32) | ((uint64_t)((w >> 2) & 0x3FFFFF) << 36);
            if (*cnt < IPC_MAXBUF) arr[(*cnt)++] = b;
        }
    }
    if (x->type > 0xF) {              /* TIPC (SDK 12+, used by sm:): type = 16 + command, raw data follows the handles unaligned */
        x->cmd = x->type - 16;
        x->in = (const uint8_t *)p;
        x->in_size = words * 4;
        return 2;
    }
    uint8_t *raw_end = (uint8_t *)p + (size_t)words * 4;
    uint8_t *raw = (uint8_t *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    if (x->type == TYPE_CLOSE) return 0;
    uint32_t raw_len = raw_end > raw ? (uint32_t)(raw_end - raw) : 0;
    /* receive list */
    int nc = rmode == 0 ? 0 : rmode == 1 ? 0 : rmode == 2 ? 1 : (int)rmode - 2;
    if (nc) {
        uint32_t *cp = roff ? m + roff : (uint32_t *)raw_end;
        for (int i = 0; i < nc; i++, cp += 2) {
            IpcBuf b;
            b.idx = (uint32_t)i;
            b.addr = cp[0] | ((uint64_t)(cp[1] & 0xFFFF) << 32);
            b.size = cp[1] >> 16;
            if (x->npc < IPC_MAXBUF) x->pc[x->npc++] = b;
        }
    }
    const uint8_t *q = raw;
    uint32_t left = raw_len;
    *domain_cmd = 0; *domain_obj = 0; *domain_data = 0;
    if (x->is_domain && (x->type == TYPE_REQUEST || x->type == TYPE_REQUEST_CTX)) {
        if (left < 16) return -1;
        *domain_cmd = q[0];
        uint32_t nin = q[1];
        *domain_data = *(const uint16_t *)(q + 2);
        *domain_obj = *(const uint32_t *)(q + 4);
        q += 16; left -= 16;
        if (*domain_cmd == 1) {      /* SendMessage: object ids follow the data */
            const uint8_t *ids = q + *domain_data;
            for (uint32_t i = 0; i < nin && i < 8; i++) memcpy(&x->in_objs[x->nin_objs++], ids + 4 * i, 4);
        } else {
            return 0;
        }
        if (left > *domain_data) left = *domain_data;
    }
    if (left < 16 || memcmp(q, "SFCI", 4) != 0) {
        rt_log("IPC: bad CMIF header (type %u raw_len %u magic %08x)", x->type, left, left >= 4 ? *(const uint32_t *)q : 0);
        return -1;
    }
    x->cmd = *(const uint32_t *)(q + 8);
    x->in = q + 16;
    x->in_size = left - 16;
    return 1;
}

/* ------------------------------------------------------------------ response */
static void ipc_respond(Ctx *c, IpcCtx *x, uint32_t result, uint32_t *obj_ids, int nobj_ids, uint32_t *obj_handles, int nobj_handles) {
    uint8_t *tls = (uint8_t *)c->tpidrro_el0;
    uint32_t *m = (uint32_t *)tls;
    memset(tls, 0, 0x100);
    uint32_t *p = m + 2;
    int ncopy = x->r_ncopy, nmove = x->r_nmove + nobj_handles;
    uint32_t w0 = 0, w1 = 0;
    if (ncopy + nmove) {
        w1 |= 0x80000000u;
        *p++ = ((uint32_t)ncopy << 1) | ((uint32_t)nmove << 5);
        for (int i = 0; i < ncopy; i++) *p++ = x->r_copy[i];
        for (int i = 0; i < x->r_nmove; i++) *p++ = x->r_move[i];
        for (int i = 0; i < nobj_handles; i++) *p++ = obj_handles[i];
    }
    w0 |= (uint32_t)x->r_nx << 16;
    for (int i = 0; i < x->r_nx; i++, p += 2) {
        IpcBuf *b = &x->r_x[i];
        p[0] = (b->idx & 0x3F) | ((uint32_t)((b->addr >> 36) & 0x3F) << 6) | ((uint32_t)((b->addr >> 32) & 0xF) << 12) | ((uint32_t)b->size << 16);
        p[1] = (uint32_t)b->addr;
    }
    uint8_t *raw = (uint8_t *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    uint8_t *q = raw;
    /* control messages (clone, convert to domain, pointer buffer size) are never wrapped in the domain framing, even inside a domain */
    int dom = x->is_domain && (x->type == TYPE_REQUEST || x->type == TYPE_REQUEST_CTX);
    if (dom) { *(uint32_t *)q = (uint32_t)nobj_ids; q += 16; }
    memcpy(q, "SFCO", 4);
    *(uint32_t *)(q + 8) = result;
    q += 16;
    uint32_t osz = (x->out_size + 3) & ~3u;
    memcpy(q, x->out, x->out_size);
    q += osz;
    if (dom) { for (int i = 0; i < nobj_ids; i++) { memcpy(q, &obj_ids[i], 4); q += 4; } }
    uint32_t words = (uint32_t)(((raw - (uint8_t *)p) + (q - raw)) / 4);
    w1 |= words & 0x3FF;
    m[0] = w0;
    m[1] = w1;
}

static void ipc_respond_tipc(Ctx *c, IpcCtx *x, uint32_t result) {
    /* A TIPC client (nn::sm::GetServiceHandle and the other sdk tipc callers) takes a failure from the return value of
     * svcSendSyncRequest and never looks at the reply words when that is nonzero; on success it re-reads fixed offsets. So an
     * error is the svc result and no reply is written (the real kernel passes the server's error code up the same way). */
    if (result) { c->x[0] = result; return; }
    uint32_t *m = (uint32_t *)c->tpidrro_el0;
    uint32_t type = m[0] & 0xFFFF;
    memset(m, 0, 0x100);
    uint32_t *p = m + 2;
    uint32_t w1 = 0;
    int ncopy = x->r_ncopy, nmove = x->r_nmove;
    if (ncopy + nmove) {
        w1 |= 0x80000000u;
        *p++ = ((uint32_t)ncopy << 1) | ((uint32_t)nmove << 5);
        for (int i = 0; i < ncopy; i++) *p++ = x->r_copy[i];
        for (int i = 0; i < nmove; i++) *p++ = x->r_move[i];
    }
    *p++ = result;
    memcpy(p, x->out, x->out_size);
    uint32_t words = 1 + ((x->out_size + 3) >> 2);
    m[0] = type;
    m[1] = w1 | (words & 0x3FF);
    if (g_trace) rt_log("  tipc reply: %08x %08x %08x %08x %08x %08x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* ------------------------------------------------------------------ control and dispatch */
static void log_call(Session *s, const char *iface, uint32_t cmd, const char *name, IpcCtx *x) {
    Cov *cv = cov_get(iface, cmd, name, 0);
    LONG64 n = ++cv->count;
    if (n == 1 || g_trace) {
        rt_log("IPC %s[%s] cmd %u %s (in %u bytes, bufs A%d B%d X%d C%d W%d, copy %d move %d)%s", iface, s->name, cmd, name, x->in_size, x->npa, x->npb, x->npx, x->npc,
               x->npw, x->ncopy, x->nmove, x->is_domain ? " [domain]" : "");
    }
}

static void do_control(Ctx *c, Session *s, IpcCtx *x) {
    switch (x->cmd) {
    case 0: {                                   /* ConvertCurrentObjectToDomain */
        if (!s->domain) { s->domain = 1; domain_add(s, s->root); }
        out_u32(x, 0, 1);
        break;
    }
    case 1: {                                   /* CopyFromCurrentDomain(object id) -> move handle */
        uint32_t id = in_u32(x, 0);
        Session *d = D(s);
        if (!s->domain || id == 0 || id > (uint32_t)d->nobjs) { x->result = RES_CMIF_BAD_OBJECT; break; }
        SvcObj *o = d->objs[id - 1];
        InterlockedIncrement(&o->refs);
        Session *ns = session_new(o->iface->name, o);
        out_move_handle(x, k_session_new(o->iface->name, ns));
        break;
    }
    case 2: case 4: {                           /* CloneCurrentObject(Ex) */
        if (s->domain) {                        /* a clone of a domain session is a session on the same domain */
            Session *d = D(s);
            Session *ns = session_new(s->name, d->objs[0]);
            ns->domain = 1; ns->dom = d;
            InterlockedIncrement(&d->refs);
            out_move_handle(x, k_session_new(s->name, ns));
            break;
        }
        SvcObj *o = s->root;
        InterlockedIncrement(&o->refs);
        Session *ns = session_new(s->name, o);
        out_move_handle(x, k_session_new(s->name, ns));
        break;
    }
    case 3:                                     /* QueryPointerBufferSize */
        out_u32(x, 0, 0x8000);
        break;
    default:
        hle_ipc_note_unknown("<control>", x->cmd);
        x->result = RES_CMIF_UNKNOWN_CMD;
        break;
    }
    (void)c;
}

void hle_ipc_send_sync(Ctx *c, uint32_t handle) {
    ipc_lock_init();
    Session *s = k_session_user(handle);
    if (!s) {
        c->x[0] = k_obj_of(handle) ? 0xF601 : RES_INVALID_HANDLE;
        rt_log("SendSyncRequest on %s handle %#x (lr=%#llx, request word0 %#x)", k_obj_of(handle) ? "non-session" : "invalid", handle, (unsigned long long)c->x[30],
               *(uint32_t *)c->tpidrro_el0);
        { uint64_t fp = c->x[29]; char bt[400]; int n = 0;
          for (int i = 0; i < 10 && fp > 0x10000 && !(fp & 7); i++) { n += snprintf(bt + n, sizeof bt - n, " %#llx", (unsigned long long)((uint64_t *)fp)[1]); fp = ((uint64_t *)fp)[0]; }
          rt_log("  backtrace (guest lr chain):%s", bt); }
        { const uint32_t *w = (const uint32_t *)c->tpidrro_el0; rt_log("  request words: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9]); }
        return;
    }
    IpcCtx *x = calloc(1, sizeof *x);
    uint8_t *tls = (uint8_t *)c->tpidrro_el0;
    x->c = c; x->sess = s;
    EnterCriticalSection(&g_ipc);
    x->is_domain = s->domain;
    uint32_t dcmd, dobj, ddata;
    int pr = ipc_parse(tls, x, &dcmd, &dobj, &ddata);
    c->x[0] = 0;
    if (x->type == TYPE_CLOSE) { LeaveCriticalSection(&g_ipc); free(x); return; }
    if (pr == 0 && x->is_domain && dcmd == 2) {          /* domain CloseObject */
        Session *d = D(s);
        if (dobj && dobj <= (uint32_t)d->nobjs) { svcobj_release(d->objs[dobj - 1]); d->objs[dobj - 1] = svcobj_new(0, 0); }
        ipc_respond(c, x, 0, 0, 0, 0, 0);
        LeaveCriticalSection(&g_ipc);
        free(x);
        return;
    }
    if (pr < 0) {
        { const uint32_t *w = (const uint32_t *)tls; rt_log("IPC: request on %s could not be parsed (%d): %08x %08x %08x %08x %08x %08x %08x %08x", s->name, pr, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]); }
        ipc_respond(c, x, RES_CMIF_UNKNOWN_CMD, 0, 0, 0, 0);
        LeaveCriticalSection(&g_ipc);
        free(x);
        return;
    }
    uint32_t obj_ids[8], obj_handles[8];
    int nid = 0, nh = 0;
    if (pr == 2) {
        SvcObj *self = s->root;
        const SvcCmd *cm = 0;
        x->self = self;
        for (int i = 0; i < self->iface->ncmds; i++) if (self->iface->cmds[i].id == x->cmd) { cm = &self->iface->cmds[i]; break; }
        if (!cm) { hle_ipc_note_unknown(self->iface->name, x->cmd); x->result = permissive() ? 0 : RES_CMIF_UNKNOWN_CMD; }
        else { log_call(s, self->iface->name, x->cmd, cm->name, x); cm->fn(x); }
        for (int i = 0; i < x->r_nobj; i++) {
            SvcObj *o = svcobj_new(x->r_obj[i].iface, x->r_obj[i].state);
            Session *ns = session_new(o->iface->name, o);
            out_move_handle(x, k_session_new(o->iface->name, ns));
        }
        if (x->result) { x->r_ncopy = 0; x->r_nmove = 0; x->out_size = 0; }
        ipc_respond_tipc(c, x, x->result);
        LeaveCriticalSection(&g_ipc);
        free(x);
        return;
    }
    if (x->type == TYPE_CONTROL || x->type == TYPE_CONTROL_CTX) {
        log_call(s, "<control>", x->cmd, "control", x);
        do_control(c, s, x);
    } else {
        SvcObj *self = s->root;
        if (x->is_domain) {
            self = (dobj && dobj <= (uint32_t)D(s)->nobjs) ? D(s)->objs[dobj - 1] : 0;
        }
        if (!self || !self->iface) {
            rt_log("IPC: request to invalid object id %u in session %s", dobj, s->name);
            x->result = RES_CMIF_BAD_OBJECT;
        } else {
            x->self = self;
            const SvcIface *f = self->iface;
            const SvcCmd *cm = 0;
            for (int i = 0; i < f->ncmds; i++) if (f->cmds[i].id == x->cmd) { cm = &f->cmds[i]; break; }
            if (!cm) {
                hle_ipc_note_unknown(f->name, x->cmd);
                x->result = permissive() ? 0 : RES_CMIF_UNKNOWN_CMD;
                if (permissive()) out_zero(x, 0, 0x40);
            } else {
                log_call(s, f->name, x->cmd, cm->name, x);
                cm->fn(x);
            }
        }
    }
    /* out objects become domain ids or new sessions */
    for (int i = 0; i < x->r_nobj; i++) {
        SvcObj *o = svcobj_new(x->r_obj[i].iface, x->r_obj[i].state);
        if (x->is_domain) obj_ids[nid++] = (uint32_t)domain_add(s, o);
        else {
            Session *ns = session_new(o->iface->name, o);
            obj_handles[nh++] = k_session_new(o->iface->name, ns);
        }
    }
    if (x->result) {
        x->r_ncopy = 0; x->r_nmove = 0; x->out_size = 0; x->r_nx = 0; nid = 0; nh = 0;
    }
    ipc_respond(c, x, x->result, obj_ids, nid, obj_handles, nh);
    LeaveCriticalSection(&g_ipc);
    free(x);
}

void hle_ipc_connect_named_port(Ctx *c, const char *name) {
    ipc_lock_init();
    if (strcmp(name, "sm:") != 0) {
        rt_log("IPC: svcConnectToNamedPort(\"%s\"): unknown named port (only sm: exists)", name);
        hle_ipc_note_unknown("<named-port>", 0);
        c->x[0] = 0xF201;          /* NotFound */
        c->x[1] = 0;
        return;
    }
    const ServiceDef *d = hle_find_service("sm:");
    if (!d) { c->x[0] = 0xF201; c->x[1] = 0; return; }
    SvcObj *o = svcobj_new(d->root, d->make_state ? d->make_state() : 0);
    Session *s = session_new("sm:", o);
    uint32_t h = k_session_new("sm:", s);
    rt_log("svcConnectToNamedPort(\"%s\") -> session handle %#x", name, h);
    c->x[0] = 0;
    c->x[1] = h;
}

/* stop-the-world support (hle_thread.c): no guest thread is suspended while it is inside a service handler */
void hle_ipc_lock(void) { ipc_lock_init(); EnterCriticalSection(&g_ipc); }
void hle_ipc_unlock(void) { LeaveCriticalSection(&g_ipc); }
