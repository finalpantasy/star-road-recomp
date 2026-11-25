/* Star Road Recomp C++ exceptions, unwind by return (task-14, docs/RECOMPILER.md 4.4). Our own code,
 * written from the public Itanium C++ ABI and DWARF EH specifications (no libc++abi or libunwind source).
 *
 * Model
 *  - __cxa_throw stores an exception record (EhExc) in c->exc and returns. Lifted code tests c->exc after
 *    every call. A call site inside an LSDA call-site range with a landing pad calls eh_land(), which walks
 *    the LSDA action chain against the thrown type at run time (catch clauses, catch (...), exception
 *    specifications, cleanups) exactly like the personality routine's phase 2, then the lifted code jumps
 *    to the pad label with x0 = record and x1 = selector. Without a pad the function returns at once.
 *  - The landing-pad x0 is our record, not an _Unwind_Exception. Only the EH ABI functions consume it
 *    (__cxa_begin_catch, _Unwind_Resume, __cxa_get_exception_ptr, __cxa_call_unexpected), and all of them
 *    are ours: main's imports through HLE_OVR, and the sdk's internal direct calls through the lifter's
 *    redirect table (eh_post_register).
 *  - Records: a primary record sits in the 0x80-byte prefix of every object from __cxa_allocate_exception;
 *    a dependent record (std::rethrow_exception) is separate and points at its primary. Reference counts
 *    follow the ABI: the in-flight or caught exception holds one, each std::exception_ptr holds one.
 *  - Single phase: a frame whose chain has a matching catch lands there; otherwise a frame with a cleanup
 *    lands with selector 0; otherwise it returns. With no handler anywhere C++ would terminate before
 *    unwinding; we run the cleanups and report the uncaught exception at the guest-call boundary
 *    (rt_call_guest), which IL2CPP never reaches because it always catches at Runtime::Invoke and thread entry.
 */
#include "eh.h"

#define EXC_HDR 0x80
#define EH_MAGIC 0x31584845u /* "EHX1" */

typedef struct EhExc {
    uint32_t magic;
    int32_t handlers;      /* handler count; negative while rethrown from inside its catch (ABI) */
    struct EhExc *primary; /* 0 for a primary record, else the primary of this dependent record */
    volatile LONG refs;    /* primary only */
    uint32_t flags;        /* 1 = prefix of an allocated object */
    uint64_t obj;          /* thrown object */
    uint64_t type;         /* std::type_info of the thrown object */
    uint64_t dtor;         /* guest destructor or 0 */
    uint64_t adjusted;     /* what __cxa_begin_catch returns for the clause that matched */
    int64_t selector;
    uint64_t throw_lr;
    struct EhExc *next;    /* caught-exceptions stack */
} EhExc;
_Static_assert(sizeof(EhExc) <= EXC_HDR, "record must fit the object prefix");

static _Thread_local EhExc *tl_caught;
static _Thread_local int tl_uncaught;

static volatile LONG64 g_n_throw, g_n_rethrow, g_n_land_catch, g_n_land_cleanup, g_n_land_spec, g_n_resume, g_n_nosite,
    g_n_interp_land, g_n_destroy, g_n_foreign;

static inline uint64_t rd64(uint64_t a) { return *(const uint64_t *)a; }
static inline uint32_t rd32(uint64_t a) { return *(const uint32_t *)a; }
static inline void wr64(uint64_t a, uint64_t v) { *(uint64_t *)a = v; }
static inline EhExc *hdr_of(uint64_t obj) { return (EhExc *)(obj - EXC_HDR); }
static inline EhExc *rec(uint64_t p) {
    EhExc *e = (EhExc *)p;
    return (p > 0x10000 && !IsBadReadPtr(e, sizeof *e) && e->magic == EH_MAGIC) ? e : 0;
}

/* ------------------------------------------------------------------ type_info */
enum { K_OTHER, K_CLASS, K_SI, K_VMI, K_PTR };
static uint64_t g_vt_class, g_vt_si, g_vt_vmi, g_vt_ptr; /* vtable address points (symbol + 16) */

const char *eh_type_name(uint64_t ti, char *buf, size_t n) {
    if (ti < 0x10000 || IsBadReadPtr((void *)ti, 16)) { snprintf(buf, n, "?"); return buf; }
    return rt_safe_str(rd64(ti + 8), buf, n);
}
static const char *tname(uint64_t ti) {
    uint64_t p = rd64(ti + 8);
    const char *s = (const char *)p;
    return (s && *s == '*') ? s + 1 : s;
}
static int is_equal(uint64_t a, uint64_t b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    const char *x = tname(a), *y = tname(b);
    return x && y && strcmp(x, y) == 0;   /* main and sdk carry separate copies of some type_infos (std::exception) */
}
static int kind(uint64_t ti) {
    uint64_t vp = rd64(ti);
    if (vp && vp == g_vt_class) return K_CLASS;
    if (vp && vp == g_vt_si) return K_SI;
    if (vp && vp == g_vt_vmi) return K_VMI;
    if (vp && vp == g_vt_ptr) return K_PTR;
    return K_OTHER;
}
static int is_class(int k) { return k == K_CLASS || k == K_SI || k == K_VMI; }

/* Public base `want` of an object of dynamic class `ti` at obj (obj may be 0): adjusted pointer in *out. */
static int find_base(uint64_t ti, uint64_t want, uint64_t obj, uint64_t *out, int depth) {
    if (depth > 40) return 0;
    if (is_equal(ti, want)) { *out = obj; return 1; }
    int k = kind(ti);
    if (k == K_SI) return find_base(rd64(ti + 16), want, obj, out, depth + 1);
    if (k != K_VMI) return 0;
    uint32_t nb = rd32(ti + 20);
    for (uint32_t i = 0; i < nb && i < 64; i++) {
        uint64_t bti = rd64(ti + 24 + 16 * i);
        int64_t of = (int64_t)rd64(ti + 32 + 16 * i);
        if (!(of & 2)) continue;                    /* not public */
        int64_t off = of >> 8;
        uint64_t bobj = 0;
        if (obj) {
            if (of & 1) off = (int64_t)rd64(rd64(obj) + (uint64_t)off);   /* virtual base: offset from the vtable */
            bobj = obj + (uint64_t)off;
        }
        if (find_base(bti, want, bobj, out, depth + 1)) return 1;
    }
    return 0;
}

/* Does `catch (catch_ti)` catch a `thrown_ti`? *adj enters as the thrown object, leaves as the catch pointer. */
static int can_catch(uint64_t catch_ti, uint64_t thrown_ti, uint64_t *adj) {
    if (!catch_ti) return 1;                        /* catch (...) */
    int kc = kind(catch_ti);
    if (is_equal(catch_ti, thrown_ti)) {
        if (kc == K_PTR) *adj = *adj ? rd64(*adj) : 0;
        return 1;
    }
    int kt = kind(thrown_ti);
    if (is_class(kc)) {
        uint64_t o;
        if (is_class(kt) && find_base(thrown_ti, catch_ti, *adj, &o, 0)) { *adj = o; return 1; }
        return 0;
    }
    if (kc == K_PTR && kt == K_PTR) {
        uint32_t cf = rd32(catch_ti + 16), tf = rd32(thrown_ti + 16);
        if ((tf & ~cf) & 7) return 0;               /* thrown const/volatile/restrict must be kept by the catch */
        uint64_t cp = rd64(catch_ti + 24), tp = rd64(thrown_ti + 24);
        uint64_t p = *adj ? rd64(*adj) : 0;
        if (is_equal(cp, tp)) { *adj = p; return 1; }
        const char *cn = tname(cp);
        const char *tn = tname(tp);
        if (cn && !strcmp(cn, "v") && tn && tn[0] != 'F') { *adj = p; return 1; }   /* catch (void *): any object pointer */
        uint64_t o;
        if (is_class(kind(tp)) && is_class(kind(cp)) && find_base(tp, cp, p, &o, 0)) { *adj = o; return 1; }
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ DWARF encodings (guest memory, absolute addresses) */
static uint64_t uleb(uint64_t *p) {
    uint64_t r = 0; int s = 0; uint8_t b;
    do { b = *(const uint8_t *)(*p)++; r |= (uint64_t)(b & 0x7F) << s; s += 7; } while (b & 0x80);
    return r;
}
static int64_t sleb(uint64_t *p) {
    int64_t r = 0; int s = 0; uint8_t b;
    do { b = *(const uint8_t *)(*p)++; r |= (int64_t)(b & 0x7F) << s; s += 7; } while (b & 0x80);
    if (s < 64 && (b & 0x40)) r |= -((int64_t)1 << s);
    return r;
}
static int enc_size(uint8_t enc) {
    switch (enc & 0x0F) { case 0x00: case 0x04: case 0x0C: return 8; case 0x02: case 0x0A: return 2; case 0x03: case 0x0B: return 4; default: return 0; }
}
/* base: datarel base (eh_frame_hdr start) */
static uint64_t rd_enc(uint64_t *p, uint8_t enc, uint64_t base) {
    if (enc == 0xFF) return 0;
    uint64_t at = *p, v;
    switch (enc & 0x0F) {
    case 0x00: case 0x04: v = rd64(at); *p += 8; break;
    case 0x0C: v = rd64(at); *p += 8; break;
    case 0x01: v = uleb(p); break;
    case 0x09: v = (uint64_t)sleb(p); break;
    case 0x02: v = *(const uint16_t *)at; *p += 2; break;
    case 0x0A: v = (uint64_t)(int64_t)*(const int16_t *)at; *p += 2; break;
    case 0x03: v = rd32(at); *p += 4; break;
    case 0x0B: v = (uint64_t)(int64_t)*(const int32_t *)at; *p += 4; break;
    default: return 0;
    }
    if (!v) return 0;
    switch (enc & 0x70) { case 0x10: v += at; break; case 0x30: v += base; break; default: break; }
    if (enc & 0x80) v = rd64(v);   /* indirect */
    return v;
}

typedef struct { uint64_t lpstart, ttbase, cs, cs_end, actions; uint8_t ttenc, csenc; } Lsda;
static void lsda_parse(uint64_t lsda, uint64_t func, Lsda *L) {
    uint64_t p = lsda;
    uint8_t lpenc = *(const uint8_t *)p++;
    L->lpstart = lpenc == 0xFF ? func : rd_enc(&p, lpenc, 0);
    L->ttenc = *(const uint8_t *)p++;
    L->ttbase = 0;
    if (L->ttenc != 0xFF) { uint64_t off = uleb(&p); L->ttbase = p + off; }
    L->csenc = *(const uint8_t *)p++;
    uint64_t len = uleb(&p);
    L->cs = p; L->cs_end = p + len; L->actions = p + len;
}
static uint64_t tt_entry(const Lsda *L, int64_t idx) {
    int sz = enc_size(L->ttenc);
    if (!sz || !L->ttbase) return 0;
    uint64_t p = L->ttbase - (uint64_t)idx * (uint64_t)sz;
    return rd_enc(&p, L->ttenc, 0);
}

/* ------------------------------------------------------------------ eh_frame_hdr index per module */
typedef struct { uint64_t hdr, table; uint32_t count; uint8_t tenc; int ok; } EhIndex;
static EhIndex g_idx[NMODULES];

static void index_module(int i) {
    Module *m = &g_mod[i];
    EhIndex *x = &g_idx[i];
    memset(x, 0, sizeof *x);
    uint32_t mod0 = rd32(m->base + 4);
    if (mod0 + 28 > m->image_size) return;
    uint64_t M = m->base + mod0;
    if (rd32(M) != 0x30444F4Du) return;           /* "MOD0" */
    int32_t eh0 = (int32_t)rd32(M + 16), eh1 = (int32_t)rd32(M + 20);
    if (eh1 <= eh0) return;
    uint64_t h = M + (int64_t)eh0, p = h + 4;
    uint8_t ver = *(uint8_t *)h, penc = *(uint8_t *)(h + 1), cenc = *(uint8_t *)(h + 2), tenc = *(uint8_t *)(h + 3);
    if (ver != 1) return;
    rd_enc(&p, penc, h);
    x->count = (uint32_t)rd_enc(&p, cenc, h);
    x->table = p; x->hdr = h; x->tenc = tenc;
    x->ok = tenc == 0x3B;
}

/* FDE for ip: function start in *fs, LSDA in *lsda (0 if none). Returns 1 if an FDE covers ip. */
static int fde_find(uint64_t ip, uint64_t *fs, uint64_t *lsda) {
    const Module *m = ld_module_of(ip);
    if (!m) return 0;
    EhIndex *x = &g_idx[m - g_mod];
    if (!x->ok || !x->count) return 0;
    const int32_t *t = (const int32_t *)x->table;
    uint32_t lo = 0, hi = x->count;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (x->hdr + (int64_t)t[2 * mid] <= ip) lo = mid; else hi = mid;
    }
    uint64_t start = x->hdr + (int64_t)t[2 * lo];
    if (start > ip) return 0;
    uint64_t fde = x->hdr + (int64_t)t[2 * lo + 1];
    uint64_t cie = fde + 4 - rd32(fde + 4);
    /* CIE augmentation */
    uint64_t p = cie + 8;
    uint8_t ver = *(uint8_t *)p++;
    const char *aug = (const char *)p;
    p += strlen(aug) + 1;
    if (ver >= 4) p += 2;
    uleb(&p); sleb(&p);
    if (ver == 1) p++; else uleb(&p);
    uint8_t fenc = 0, lenc = 0xFF;
    if (aug[0] == 'z') {
        uint64_t alen = uleb(&p), end = p + alen;
        for (const char *a = aug + 1; *a; a++) {
            if (*a == 'R') fenc = *(uint8_t *)p++;
            else if (*a == 'L') lenc = *(uint8_t *)p++;
            else if (*a == 'P') { uint8_t pe = *(uint8_t *)p++; rd_enc(&p, (uint8_t)(pe & 0x7F), 0); }
        }
        p = end;
    }
    uint64_t q = fde + 8;
    uint64_t pc = rd_enc(&q, fenc, 0);
    uint64_t rng = rd_enc(&q, (uint8_t)(fenc & 0x0F), 0);
    if (ip < pc || ip >= pc + rng) return 0;
    *fs = pc;
    *lsda = 0;
    if (aug[0] == 'z') {
        uleb(&q);
        if (lenc != 0xFF) *lsda = rd_enc(&q, lenc, 0);
    }
    return 1;
}

int eh_lookup_site(uint64_t ra, uint64_t *pad, uint64_t *lsda, uint64_t *action) {
    uint64_t fs, ls, ip = ra - 1;
    *pad = *lsda = *action = 0;
    if (!fde_find(ip, &fs, &ls) || !ls) return 0;
    Lsda L;
    lsda_parse(ls, fs, &L);
    uint64_t p = L.cs;
    while (p < L.cs_end) {
        uint64_t s = rd_enc(&p, (uint8_t)(L.csenc & 0x0F), 0), n = rd_enc(&p, (uint8_t)(L.csenc & 0x0F), 0);
        uint64_t lp = rd_enc(&p, (uint8_t)(L.csenc & 0x0F), 0), act = uleb(&p);
        uint64_t off = ip - L.lpstart;
        if (off < s) break;
        if (off < s + n) {
            *lsda = ls;
            if (!lp) return 2;
            *pad = L.lpstart + lp;
            *action = act ? L.actions + act - 1 : 0;
            return 1;
        }
    }
    *lsda = ls;
    return 3;
}

/* ------------------------------------------------------------------ landing (phase 2 of the personality, run per frame) */
static int spec_allows(const Lsda *L, int64_t filter, uint64_t thrown, uint64_t obj) {
    uint64_t p = L->ttbase + (uint64_t)(-filter - 1);
    for (;;) {
        uint64_t idx = uleb(&p);
        if (!idx) return 0;
        uint64_t adj = obj;
        uint64_t ti = tt_entry(L, (int64_t)idx);
        if (can_catch(ti, thrown, &adj)) return 1;
    }
}

int eh_land(Ctx *c, uint64_t lsda, uint64_t action) {
    EhExc *e = rec((uint64_t)c->exc);
    if (!e) { InterlockedIncrement64(&g_n_foreign); rt_log("eh: c->exc %p is not an exception record at a landing pad", c->exc); return 0; }
    int64_t sel = 0;
    int cleanup = 0, found = 0;
    if (action) {
        Lsda L;
        lsda_parse(lsda, 0, &L);
        uint64_t p = action;
        for (int guard = 0; guard < 4096; guard++) {
            int64_t filter = sleb(&p);
            uint64_t nx = p;
            int64_t next = sleb(&nx);
            if (filter > 0) {
                uint64_t adj = e->obj;
                if (can_catch(tt_entry(&L, filter), e->type, &adj)) { e->adjusted = adj; sel = filter; found = 1; break; }
            } else if (filter < 0) {
                if (!spec_allows(&L, filter, e->type, e->obj)) { e->adjusted = e->obj; sel = filter; found = 1; InterlockedIncrement64(&g_n_land_spec); break; }
            } else {
                cleanup = 1;
            }
            if (!next) break;
            p = p + (uint64_t)next;   /* p is still the address of the next-offset field: offsets are relative to it */
        }
    } else {
        cleanup = 1;
    }
    if (!found && !cleanup) return 0;
    if (found && sel > 0) InterlockedIncrement64(&g_n_land_catch);
    else if (!found) InterlockedIncrement64(&g_n_land_cleanup);
    e->selector = found ? sel : 0;
    c->x[0] = (uint64_t)e;
    c->x[1] = (uint64_t)e->selector;
    c->exc = 0;
    c->exc_type = 0;
    return 1;
}

void eh_no_site(Ctx *c, uint64_t call_addr) {
    LONG64 n = InterlockedIncrement64(&g_n_nosite);
    if (n <= 50) {
        char nm[160];
        EhExc *e = rec((uint64_t)c->exc);
        rt_log("eh: exception %s passed a call with no call-site entry at %#llx (C++ would call std::terminate); propagating",
               e ? eh_type_name(e->type, nm, sizeof nm) : "?", (unsigned long long)call_addr);
    }
}

int eh_interp_land(Ctx *c, uint64_t ra, uint64_t *pad) {
    uint64_t lsda, action;
    int r = eh_lookup_site(ra, pad, &lsda, &action);
    if (r == 3) { eh_no_site(c, ra - 4); return 0; }
    if (r != 1) return 0;
    if (!eh_land(c, lsda, action)) return 0;
    InterlockedIncrement64(&g_n_interp_land);
    return 1;
}

/* ------------------------------------------------------------------ records and reference counts */
static void destroy(Ctx *c, EhExc *p) {
    InterlockedIncrement64(&g_n_destroy);
    if (p->dtor) {
        void *keep = c->exc;
        uint64_t keept = c->exc_type, lr = c->x[30], x0 = c->x[0], x1 = c->x[1];
        c->exc = 0;
        c->x[0] = p->obj;
        rt_icall(c, p->dtor);
        c->exc = keep; c->exc_type = keept; c->x[30] = lr; c->x[0] = x0; c->x[1] = x1;
    }
    if (p->flags & 1) { p->magic = 0; free(p); }
}
static void decref(Ctx *c, EhExc *p) { if (p && InterlockedDecrement(&p->refs) == 0) destroy(c, p); }
static void incref_obj(uint64_t obj) { if (obj) InterlockedIncrement(&hdr_of(obj)->refs); }
static void decref_obj(Ctx *c, uint64_t obj) { if (obj) decref(c, hdr_of(obj)); }
static void release(Ctx *c, EhExc *e) {
    if (e->primary) { EhExc *p = e->primary; e->magic = 0; free(e); decref(c, p); }
    else decref(c, e);
}
static EhExc *primary_of(EhExc *e) { return e->primary ? e->primary : e; }

static void log_throw(Ctx *c, EhExc *e, const char *what) {
    LONG64 n = InterlockedIncrement64(what[0] == 't' ? &g_n_throw : &g_n_rethrow);
    if (g_trace || n <= 20) {
        char nm[160];
        rt_log("C++ %s: object=%#llx type=%s lr=%#llx", what, (unsigned long long)e->obj, eh_type_name(e->type, nm, sizeof nm),
               (unsigned long long)c->x[30]);
    }
}
static void raise_rec(Ctx *c, EhExc *e) {
    c->exc = e;
    c->exc_type = e->type;
    tl_uncaught++;
}

/* ------------------------------------------------------------------ the ABI functions */
typedef struct { const char *name; GuestFn fn; } EhName;
static EhName g_names[64];
static int g_nnames;
static void eh_name_add(const char *n, GuestFn f) { if (g_nnames < RT_ARRAY_LEN(g_names)) g_names[g_nnames++] = (EhName){n, f}; }
static GuestFn eh_name_fn(const char *n) {
    for (int i = 0; i < g_nnames; i++) if (!strcmp(g_names[i].name, n)) return g_names[i].fn;
    return 0;
}
/* Registered before hle_nn.c's default-priority constructors, so hle_find() (first match) picks these. */
#define EH_HLE(id, sym) \
    static void ehh_##id(Ctx *c); \
    __attribute__((constructor(150))) static void ehreg_##id(void) { hle_add(sym, ehh_##id, HLE_F_OVERRIDE); eh_name_add(sym, ehh_##id); } \
    static void ehh_##id(Ctx *c)
/* sdk-internal names only (no import of them exists; reached through the redirect table) */
#define EH_INT(id, sym) \
    static void ehh_##id(Ctx *c); \
    __attribute__((constructor(150))) static void ehreg_##id(void) { eh_name_add(sym, ehh_##id); } \
    static void ehh_##id(Ctx *c)

EH_HLE(alloc, "__cxa_allocate_exception") {
    char *p = calloc(1, EXC_HDR + c->x[0] + 16);
    if (!p) rt_fatal(c, "__cxa_allocate_exception: out of memory", c->x[0]);
    EhExc *e = (EhExc *)p;
    e->magic = EH_MAGIC; e->flags = 1; e->obj = (uint64_t)(p + EXC_HDR);
    c->x[0] = e->obj;
}
EH_HLE(free, "__cxa_free_exception") { if (c->x[0]) { EhExc *e = hdr_of(c->x[0]); e->magic = 0; free(e); } }
EH_HLE(throw, "__cxa_throw") {
    EhExc *e = hdr_of(c->x[0]);
    e->magic = EH_MAGIC; e->handlers = 0; e->primary = 0; e->refs = 1;
    e->type = c->x[1]; e->dtor = c->x[2]; e->adjusted = e->obj; e->throw_lr = c->x[30];
    log_throw(c, e, "throw");
    raise_rec(c, e);
}
EH_HLE(rethrow, "__cxa_rethrow") {
    EhExc *e = tl_caught;
    if (!e) rt_fatal(c, "__cxa_rethrow with no caught exception (std::terminate)", c->x[30]);
    e->handlers = -e->handlers;
    log_throw(c, e, "rethrow");
    raise_rec(c, e);
}
EH_HLE(begin, "__cxa_begin_catch") {
    EhExc *e = rec(c->x[0]);
    if (!e) { InterlockedIncrement64(&g_n_foreign); rt_log("eh: __cxa_begin_catch(%#llx) is not an exception record", (unsigned long long)c->x[0]); return; }
    e->handlers = e->handlers < 0 ? -e->handlers + 1 : e->handlers + 1;
    if (e != tl_caught) { e->next = tl_caught; tl_caught = e; }
    tl_uncaught--;
    c->exc = 0;
    c->x[0] = e->adjusted;
}
EH_HLE(end, "__cxa_end_catch") {
    EhExc *e = tl_caught;
    if (!e) return;
    if (e->handlers < 0) {
        if (++e->handlers == 0) tl_caught = e->next;
    } else if (--e->handlers == 0) {
        tl_caught = e->next;
        release(c, e);
    }
}
EH_HLE(getptr, "__cxa_get_exception_ptr") { EhExc *e = rec(c->x[0]); if (e) c->x[0] = e->adjusted; }
EH_HLE(curtype, "__cxa_current_exception_type") { c->x[0] = tl_caught ? tl_caught->type : 0; }
EH_HLE(uncaught, "__cxa_uncaught_exception") { c->x[0] = tl_uncaught != 0; }
EH_HLE(uncaughts, "__cxa_uncaught_exceptions") { c->x[0] = (uint64_t)(uint32_t)tl_uncaught; }
EH_HLE(resume, "_Unwind_Resume") {
    EhExc *e = rec(c->x[0]);
    if (!e) rt_fatal(c, "_Unwind_Resume with a value that is not an exception record", c->x[0]);
    InterlockedIncrement64(&g_n_resume);
    c->exc = e;
    c->exc_type = e->type;
}
EH_HLE(personality, "__gxx_personality_v0") { rt_fatal(c, "__gxx_personality_v0 called: landing pads are resolved by the lifter", c->x[30]); }
EH_HLE(terminate, "_ZSt9terminatev") { rt_fatal(c, "std::terminate called", c->x[30]); }
EH_HLE(unexpected, "__cxa_call_unexpected") {
    char nm[160];
    EhExc *e = rec(c->x[0]);
    rt_log("eh: exception %s violated an exception specification", e ? eh_type_name(e->type, nm, sizeof nm) : "?");
    rt_fatal(c, "__cxa_call_unexpected (std::unexpected, then std::terminate)", c->x[0]);
}
/* std::exception_ptr (libc++: one pointer to the primary thrown object) */
EH_HLE(ep_copy, "_ZNSt13exception_ptrC1ERKS_") { uint64_t p = rd64(c->x[1]); wr64(c->x[0], p); incref_obj(p); }
EH_HLE(ep_copy2, "_ZNSt13exception_ptrC2ERKS_") { uint64_t p = rd64(c->x[1]); wr64(c->x[0], p); incref_obj(p); }
EH_HLE(ep_dtor, "_ZNSt13exception_ptrD1Ev") { uint64_t p = rd64(c->x[0]); wr64(c->x[0], 0); decref_obj(c, p); }
EH_HLE(ep_dtor2, "_ZNSt13exception_ptrD2Ev") { uint64_t p = rd64(c->x[0]); wr64(c->x[0], 0); decref_obj(c, p); }
EH_HLE(ep_assign, "_ZNSt13exception_ptraSERKS_") {
    uint64_t self = c->x[0], np = rd64(c->x[1]), op = rd64(self);
    incref_obj(np);
    wr64(self, np);
    decref_obj(c, op);
    c->x[0] = self;
}
EH_HLE(ep_current, "_ZSt17current_exceptionv") {   /* result through x8 (non-trivial return type) */
    uint64_t p = tl_caught ? primary_of(tl_caught)->obj : 0;
    incref_obj(p);
    wr64(c->x[8], p);
}
static void rethrow_primary(Ctx *c, uint64_t obj) {
    if (!obj) rt_fatal(c, "std::rethrow_exception with a null exception_ptr (std::terminate)", c->x[30]);
    EhExc *p = hdr_of(obj);
    EhExc *d = calloc(1, sizeof *d);
    d->magic = EH_MAGIC; d->primary = p; d->obj = obj; d->type = p->type; d->adjusted = obj; d->throw_lr = c->x[30];
    InterlockedIncrement(&p->refs);
    log_throw(c, d, "rethrow_exception");
    raise_rec(c, d);
}
EH_HLE(ep_rethrow, "_ZSt17rethrow_exceptionSt13exception_ptr") { rethrow_primary(c, rd64(c->x[0])); }   /* by invisible reference */
EH_INT(cur_primary, "__cxa_current_primary_exception") {
    uint64_t p = tl_caught ? primary_of(tl_caught)->obj : 0;
    incref_obj(p);
    c->x[0] = p;
}
EH_INT(rethrow_primary, "__cxa_rethrow_primary_exception") { if (c->x[0]) rethrow_primary(c, c->x[0]); }
EH_INT(incref, "__cxa_increment_exception_refcount") { incref_obj(c->x[0]); }
EH_INT(decref, "__cxa_decrement_exception_refcount") { decref_obj(c, c->x[0]); }
EH_INT(raise, "_Unwind_RaiseException") { rt_fatal(c, "_Unwind_RaiseException reached (an EH entry point is missing from the redirect table)", c->x[30]); }
EH_INT(resume_or_rethrow, "_Unwind_Resume_or_Rethrow") { rt_fatal(c, "_Unwind_Resume_or_Rethrow reached", c->x[30]); }

/* ------------------------------------------------------------------ setup, self test, summary */
static void selftest(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { rt_log("eh selftest: cannot open %s", path); return; }
    char ln[256];
    long n = 0, bad = 0;
    while (fgets(ln, sizeof ln, f)) {
        unsigned long long ra, pad, act;
        int kind;
        if (sscanf(ln, "%llx %d %llx %llx", &ra, &kind, &pad, &act) != 4) continue;
        uint64_t p, l, a;
        int r = eh_lookup_site(ra, &p, &l, &a);
        n++;
        if (r != kind || (r == 1 && (p != pad || a != act))) {
            if (++bad <= 20) rt_log("eh selftest MISMATCH ra=%#llx lifter kind %d pad %#llx action %#llx, runtime kind %d pad %#llx action %#llx",
                                    ra, kind, pad, act, r, (unsigned long long)p, (unsigned long long)a);
        }
    }
    fclose(f);
    rt_log("eh selftest: %ld call sites compared with the runtime LSDA reader, %ld mismatches", n, bad);
}

void eh_post_register(const RtRedirect *r, uint32_t n) {
    uint64_t a;
    if ((a = ld_find_export("_ZTVN10__cxxabiv117__class_type_infoE"))) g_vt_class = a + 16;
    if ((a = ld_find_export("_ZTVN10__cxxabiv120__si_class_type_infoE"))) g_vt_si = a + 16;
    if ((a = ld_find_export("_ZTVN10__cxxabiv121__vmi_class_type_infoE"))) g_vt_vmi = a + 16;
    if ((a = ld_find_export("_ZTVN10__cxxabiv119__pointer_type_infoE"))) g_vt_ptr = a + 16;
    int nidx = 0;
    for (int i = 0; i < NMODULES; i++) { index_module(i); nidx += g_idx[i].ok; }
    uint32_t nred = 0, nmiss = 0;
    for (uint32_t i = 0; i < n; i++) {
        GuestFn f = eh_name_fn(r[i].name);
        if (f) { rt_reg(r[i].addr, f); nred++; }
        else { nmiss++; rt_log("eh: redirect target %s at %#llx has no runtime implementation", r[i].name, (unsigned long long)r[i].addr); }
    }
    const HleDef *d = hle_find("__cxa_throw");
    int ours = d && d->fn == ehh_throw;
    rt_log("eh: runtime/eh active=%s (HLE_OVR order), vtables class %s si %s vmi %s ptr %s, eh_frame_hdr indexed %d/%d modules (FDEs main %u sdk %u subsdk0 %u), sdk redirects %u (missing %u)",
           ours ? "yes" : "NO, another __cxa_throw HLE was registered first", g_vt_class ? "ok" : "MISSING", g_vt_si ? "ok" : "MISSING",
           g_vt_vmi ? "ok" : "MISSING", g_vt_ptr ? "ok" : "MISSING", nidx, NMODULES, g_idx[0].count, g_idx[1].count, g_idx[2].count, nred, nmiss);
    const char *st = getenv("SMRPG_EH_SELFTEST");
    if (st && *st) selftest(st);
}

void eh_summary(void) {
    rt_log("eh: throws %lld, rethrows %lld, landed catch %lld cleanup %lld spec %lld (interpreter %lld), _Unwind_Resume %lld, destroyed %lld, no-site %lld, foreign %lld",
           (long long)g_n_throw, (long long)g_n_rethrow, (long long)g_n_land_catch, (long long)g_n_land_cleanup, (long long)g_n_land_spec,
           (long long)g_n_interp_land, (long long)g_n_resume, (long long)g_n_destroy, (long long)g_n_nosite, (long long)g_n_foreign);
}
