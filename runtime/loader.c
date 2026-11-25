/* Star Road Recomp runtime loader (task-5): maps the prepared module images at their fixed bases,
 * resolves symbols across modules and the HLE table, applies relocations. Our own code. */
#include "rt_internal.h"

Module g_mod[NMODULES] = {{.name = "main", .base = GUEST_MOD_MAIN}, {.name = "sdk", .base = GUEST_MOD_SDK}, {.name = "subsdk0", .base = GUEST_MOD_SUB0}};

void rt_fn_commit_module(int m, uint64_t image_bytes);

/* ------------------------------------------------------------------ regions */
static volatile LONG64 g_stack_next;

/* The guest ranges are fixed (the game sees these addresses). With high-entropy ASLR the kernel puts the main thread
 * stack (and other bottom-up regions) anywhere in the low 1 TB before any of our code runs, so about 1 start in 10 found
 * a guest range occupied (error 487). The root fix is linking with -Wl,--disable-high-entropy-va (lld-link: /HIGHENTROPYVA:NO), which keeps bottom-up regions in
 * the low gigabytes. rt_reserve_early() runs first in main(): it reserves the heap and stack ranges, checks that the
 * module slots are free, and, if a range is taken anyway (an exe still linked with high-entropy VA), relaunches the
 * process (bounded by SMRPG_RESERVE_TRY) inside a kill-on-close job, so a fresh layout is tried. */
static int g_reserve_ok;
static char g_reserve_diag[1024];
static int g_reserve_he;   /* 1 if this exe was linked with high-entropy VA */

static void describe_range(char *o, size_t n, const char *what, uint64_t b, uint64_t e) {
    ULONG_PTR slo = 0, shi = 0;
    GetCurrentThreadStackLimits(&slo, &shi);
    size_t k = strlen(o);
    k += snprintf(o + k, n - k, " %s %#llx-%#llx occupied:", what, (unsigned long long)b, (unsigned long long)e);
    int shown = 0;
    for (uint64_t a = b; a < e && shown < 4 && k < n;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
        uint64_t rb = (uint64_t)mi.BaseAddress, re = rb + mi.RegionSize;
        if (mi.State != MEM_FREE) {
            const char *who = "unknown";
            char mod[MAX_PATH] = "";
            if (rb < shi && re > slo) who = "main thread stack";
            else if ((HANDLE)mi.AllocationBase == GetProcessHeap()) who = "process heap";
            else if (mi.Type == MEM_IMAGE && GetModuleFileNameA((HMODULE)mi.AllocationBase, mod, sizeof mod)) who = mod;
            else if (mi.Type == MEM_MAPPED) who = "mapped section";
            else if (mi.Type == MEM_PRIVATE) who = "private allocation";
            k += snprintf(o + k, n - k, " [%#llx +%#llx alloc %#llx state %#lx type %#lx: %s]", (unsigned long long)rb,
                          (unsigned long long)mi.RegionSize, (unsigned long long)(uintptr_t)mi.AllocationBase, mi.State, mi.Type, who);
            shown++;
        }
        a = re;
    }
}

static int range_free(uint64_t b, uint64_t e) {
    for (uint64_t a = b; a < e;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_FREE) return 0;
        a = (uint64_t)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}

static void relaunch(void) {
    char v[16];
    DWORD n = GetEnvironmentVariableA("SMRPG_RESERVE_TRY", v, sizeof v);
    int t = (n && n < sizeof v) ? atoi(v) : 0;
    if (t >= 8) return;                                      /* 8 fresh layouts all failed: report the FATAL below */
    snprintf(v, sizeof v, "%d", t + 1);
    SetEnvironmentVariableA("SMRPG_RESERVE_TRY", v);
    HANDLE job = CreateJobObjectA(NULL, NULL);               /* the child dies with us if a harness kills our PID */
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    STARTUPINFOW si = {0};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE hs[3] = {si.hStdInput, si.hStdOutput, si.hStdError};
    for (int i = 0; i < 3; i++)
        if (hs[i] && hs[i] != INVALID_HANDLE_VALUE) SetHandleInformation(hs[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, GetCommandLineW(), NULL, NULL, TRUE, CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi))
        return;
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 10;
    GetExitCodeProcess(pi.hProcess, &rc);
    ExitProcess(rc);
}

void rt_reserve_early(void) {
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const char *)dos + dos->e_lfanew);
    g_reserve_he = (nt->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA) != 0;
    void *h = VirtualAlloc((void *)GUEST_HEAP_BASE, GUEST_HEAP_SIZE, MEM_RESERVE, PAGE_READWRITE);
    void *s = VirtualAlloc((void *)GUEST_STACK_BASE, GUEST_STACKRGN_SIZE, MEM_RESERVE, PAGE_READWRITE);
    uint64_t mb = GUEST_MOD_MAIN, me = GUEST_HLE_BASE + GUEST_MOD_SPAN;   /* main, sdk, subsdk0 and HLE slots */
    int mods = range_free(mb, me);
    g_reserve_ok = h && s && mods;
    if (g_reserve_ok) return;
    g_reserve_diag[0] = 0;
    if (!h) describe_range(g_reserve_diag, sizeof g_reserve_diag, "heap", GUEST_HEAP_BASE, GUEST_HEAP_BASE + GUEST_HEAP_SIZE);
    if (!s) describe_range(g_reserve_diag, sizeof g_reserve_diag, "stacks", GUEST_STACK_BASE, GUEST_STACK_BASE + GUEST_STACKRGN_SIZE);
    if (!mods) describe_range(g_reserve_diag, sizeof g_reserve_diag, "modules", mb, me);
    if (h) VirtualFree(h, 0, MEM_RELEASE);
    if (s) VirtualFree(s, 0, MEM_RELEASE);
    SetEnvironmentVariableA("SMRPG_RESERVE_DIAG", g_reserve_diag);   /* the child logs what the collision was */
    relaunch();                                            /* returns only if relaunching is exhausted or failed */
}

void rt_reserve_regions(void) {
    const char *t = getenv("SMRPG_RESERVE_TRY");
    if (!g_reserve_ok) {
        rt_log("FATAL cannot reserve guest heap at %#llx (error 487, high-entropy VA %s, relaunches %s):%s", (unsigned long long)GUEST_HEAP_BASE,
               g_reserve_he ? "on: link with -Wl,--disable-high-entropy-va" : "off", t ? t : "0", g_reserve_diag);
        rt_exit(10, "heap reserve");
    }
    g_stack_next = (LONG64)GUEST_STACK_BASE;
    const char *d = getenv("SMRPG_RESERVE_DIAG");
    if (t && d && *d) rt_log("guest range collision on the previous start (relaunched):%s", d);
    rt_log("reserved guest heap/alias %#llx +%#llx, stacks %#llx +%#llx (high-entropy VA %s, relaunches %s)", (unsigned long long)GUEST_HEAP_BASE,
           (unsigned long long)GUEST_HEAP_SIZE, (unsigned long long)GUEST_STACK_BASE, (unsigned long long)GUEST_STACKRGN_SIZE,
           g_reserve_he ? "on" : "off", t ? t : "0");
}

uint64_t rt_stack_alloc(uint64_t size, uint64_t align) {
    size = (size + 0xFFF) & ~0xFFFULL;
    if (align < 0x1000) align = 0x1000;
    for (;;) {
        LONG64 cur = g_stack_next;
        uint64_t a = ((uint64_t)cur + align - 1) & ~(align - 1);
        if (a + size > GUEST_STACK_BASE + GUEST_STACKRGN_SIZE) return 0;
        if (InterlockedCompareExchange64(&g_stack_next, (LONG64)(a + size), cur) != cur) continue;
        if (!VirtualAlloc((void *)a, size, MEM_COMMIT, PAGE_READWRITE)) return 0;
        return a;
    }
}

const Module *ld_module_of(uint64_t a) {
    for (int i = 0; i < NMODULES; i++)
        if (g_mod[i].image_size && a >= g_mod[i].base && a < g_mod[i].base + g_mod[i].image_size) return &g_mod[i];
    return 0;
}

/* ------------------------------------------------------------------ files */
static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    b[n] = 0;
    fclose(f);
    if (len) *len = (size_t)n;
    return b;
}

static long long jnum(const char *js, const char *sec, const char *key) {
    const char *p = js;
    if (sec) {
        char k[64];
        snprintf(k, sizeof k, "\"%s\"", sec);
        p = strstr(js, k);
        if (!p) return -1;
    }
    char k[64];
    snprintf(k, sizeof k, "\"%s\":", key);
    p = strstr(p, k);
    if (!p) return -1;
    return strtoll(p + strlen(k), 0, 10);
}

/* ------------------------------------------------------------------ export table */
typedef struct { const char *name; uint64_t addr; uint8_t type, mod; } Exp;
#define EXP_CAP (1u << 17)
static Exp g_exp[EXP_CAP];
static uint32_t g_nexp;

static uint64_t fnv(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

static void exp_add(const char *name, uint64_t addr, uint8_t type, uint8_t mod) {
    uint32_t h = (uint32_t)fnv(name) & (EXP_CAP - 1);
    while (g_exp[h].name) {
        if (!strcmp(g_exp[h].name, name)) return;   /* first definition (main, sdk, subsdk0) wins */
        h = (h + 1) & (EXP_CAP - 1);
    }
    g_exp[h] = (Exp){name, addr, type, mod};
    g_nexp++;
}

static const Exp *exp_find(const char *name) {
    uint32_t h = (uint32_t)fnv(name) & (EXP_CAP - 1);
    while (g_exp[h].name) {
        if (!strcmp(g_exp[h].name, name)) return &g_exp[h];
        h = (h + 1) & (EXP_CAP - 1);
    }
    return 0;
}

uint64_t ld_find_export(const char *name) {
    const Exp *e = exp_find(name);
    return e ? e->addr : 0;
}

/* ------------------------------------------------------------------ loading */
static const RelRec *g_rels[NMODULES];

static int load_module(int i, const char *dir) {
    Module *m = &g_mod[i];
    char p[600];
    snprintf(p, sizeof p, "%s\\%s.json", dir, m->name);
    char *js = read_file(p, 0);
    if (!js) { rt_log("FATAL cannot read %s", p); return -1; }
    m->image_size = (uint32_t)jnum(js, 0, "image_size");
    m->text_size = (uint32_t)jnum(js, 0, "text_size");
    m->rodata_off = (uint32_t)jnum(js, 0, "rodata_off");
    m->rodata_size = (uint32_t)jnum(js, 0, "rodata_size");
    m->data_off = (uint32_t)jnum(js, 0, "data_off");
    m->data_size = (uint32_t)jnum(js, 0, "data_size");
    m->init_off = (uint32_t)jnum(js, "init_array", "off");
    m->init_sz = (uint32_t)jnum(js, "init_array", "size");
    m->fini_off = (uint32_t)jnum(js, "fini_array", "off");
    m->fini_sz = (uint32_t)jnum(js, "fini_array", "size");
    free(js);
    if (!m->image_size) { rt_log("FATAL bad json for %s", m->name); return -1; }
    if (m->image_size > GUEST_MOD_SPAN) { rt_log("FATAL %s image larger than its slot", m->name); return -1; }

    uint64_t sz = ((uint64_t)m->image_size + 0xFFF) & ~0xFFFULL;
    void *mem = VirtualAlloc((void *)m->base, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem != (void *)m->base) {
        rt_log("FATAL cannot map %s at %#llx (error %lu): guest range is occupied", m->name, (unsigned long long)m->base, GetLastError());
        return -1;
    }
    snprintf(p, sizeof p, "%s\\%s.img", dir, m->name);
    FILE *f = fopen(p, "rb");
    if (!f) { rt_log("FATAL cannot open %s", p); return -1; }
    size_t got = fread((void *)m->base, 1, m->image_size, f);
    fclose(f);
    if (got != m->image_size) { rt_log("FATAL %s: read %zu of %u bytes", p, got, m->image_size); return -1; }

    snprintf(p, sizeof p, "%s\\%s.sym", dir, m->name);
    uint8_t *sb = read_file(p, 0);
    if (!sb || memcmp(sb, "SYM0", 4)) { rt_log("FATAL cannot read %s", p); return -1; }
    m->nsym = *(uint32_t *)(sb + 4);
    m->strsz = *(uint32_t *)(sb + 8);
    m->syms = (const SymRec *)(sb + 16);
    m->strtab = (const char *)(sb + 16 + (size_t)m->nsym * sizeof(SymRec));
    snprintf(p, sizeof p, "%s\\%s.rel", dir, m->name);
    uint8_t *rb = read_file(p, 0);
    if (!rb || memcmp(rb, "REL0", 4)) { rt_log("FATAL cannot read %s", p); return -1; }
    m->nrel = *(uint32_t *)(rb + 4);
    g_mod[i].n_unresolved = 0;
    g_rels[i] = (const RelRec *)(rb + 16);
    rt_fn_commit_module(i, m->text_size ? m->text_size : m->image_size);
    rt_log("module %-7s mapped at %#llx image=%#x text=%#x relocs=%u dynsyms=%u init_array=%u fini_array=%u", m->name,
           (unsigned long long)m->base, m->image_size, m->text_size, m->nrel, m->nsym, m->init_sz / 8, m->fini_sz / 8);
    return 0;
}

static int is_data_symbol(const char *n) {
    return n[0] == '_' && n[1] == 'Z' && n[2] == 'T' && (n[3] == 'V' || n[3] == 'I' || n[3] == 'S' || n[3] == 'T' || n[3] == 'C');
}

static uint64_t zero_block(void) {
    static uint64_t b;
    if (!b) b = (uint64_t)VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return b;
}

static uint64_t resolve(int mi, uint32_t si, int verbose) {
    Module *m = &g_mod[mi];
    const SymRec *s = &m->syms[si];
    const char *nm = m->strtab + s->name_off;
    if (!nm[0] || s->name_off == 0) return 0;
    if (s->defined && s->bind == 0) return m->base + s->value;    /* STB_LOCAL */
    const HleDef *h = hle_find(nm);
    /* the sdk carries the console's real NVN driver (it talks to nvdrv), so our NVN layer must win over the sdk's own export */
    if (h && (!strcmp(nm, "nvnBootstrapLoader") || (h->flags & HLE_F_OVERRIDE))) { m->n_hle++; return hle_slot(nm, SLOT_HLE, h->fn, 0)->addr; }
    const Exp *e = exp_find(nm);
    if (e) {
        if (mi == 0 && e->mod != 0 && e->type == 2 && hle_is_nn_service(nm)) {
            m->n_wrapped++;
            return hle_slot(nm, SLOT_WRAP, 0, e->addr)->addr;
        }
        return e->addr;
    }
    if (h) { m->n_hle++; return hle_slot(nm, SLOT_HLE, h->fn, 0)->addr; }
    if (s->bind == 2) return 0;                                    /* undefined weak */
    if (is_data_symbol(nm)) { if (verbose) rt_log("  data import %s: zeroed block", nm); return zero_block(); }
    m->n_unresolved++;
    HleSlot *sl = hle_slot(nm, SLOT_STUB, 0, 0);
    if (verbose) rt_log("  STUB import %s (%s)", nm, m->name);
    return sl->addr;
}

static void relocate(int mi) {
    Module *m = &g_mod[mi];
    const RelRec *r = g_rels[mi];
    uint64_t *cache = calloc(m->nsym ? m->nsym : 1, 8);
    uint8_t *done = calloc(m->nsym ? m->nsym : 1, 1);
    long applied[1030] = {0};
    for (uint32_t i = 0; i < m->nrel; i++) {
        uint64_t v;
        if (r[i].type == 1027) {
            v = m->base + (uint64_t)r[i].addend;
        } else {
            uint32_t si = r[i].sym;
            if (si >= m->nsym) continue;
            if (!done[si]) { cache[si] = resolve(mi, si, g_trace); done[si] = 1; }
            v = cache[si] + (uint64_t)r[i].addend;
        }
        if (r[i].off + 8 > m->image_size) continue;
        *(uint64_t *)(m->base + r[i].off) = v;
        if (r[i].type < 1030) applied[r[i].type]++;
    }
    rt_log("relocated %-7s: RELATIVE=%ld ABS64=%ld GLOB_DAT=%ld JUMP_SLOT=%ld; imports resolved to HLE=%u wrapped-lifted=%u unresolved->stub=%u",
           m->name, applied[1027], applied[257], applied[1025], applied[1026], m->n_hle, m->n_wrapped, m->n_unresolved);
    free(cache);
    free(done);
}

/* On the console the system loader (rtld) fills a few .bss pointers the sdk's nn::ro code owns (the manual and auto load lists and the
 * slots that hold the symbol lookup function pointers) before any module code runs; nn::ro::Initialize then stores through them. rtld is
 * not loaded here, so point each one at its own zeroed cell, which is all the sdk needs while the game loads no NRO modules. */
static void provide_rtld_vars(void) {
    static const char *const names[] = {
        "_ZN2nn2ro6detail36g_pLookupGlobalManualFunctionPointerE", "_ZN2nn2ro6detail34g_pLookupGlobalAutoFunctionPointerE",
        "_ZN2nn2ro6detail17g_pManualLoadListE", "_ZN2nn2ro6detail15g_pAutoLoadListE", "_ZN2nn2ro6detail14g_pRoDebugFlagE"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        const Exp *e = exp_find(names[i]);
        if (!e || e->type != 1) continue;
        uint64_t *slot = (uint64_t *)e->addr;
        if (*slot) continue;
        void *cell = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        *slot = (uint64_t)cell;
        rt_log("rtld variable %s -> zeroed cell %p", names[i], cell);
    }
}

char g_imgdir[512];

int ld_load_all(const char *dir) {
    snprintf(g_imgdir, sizeof g_imgdir, "%s", dir);
    for (int i = 0; i < NMODULES; i++)
        if (load_module(i, dir)) return -1;
    for (int i = 0; i < NMODULES; i++) {
        Module *m = &g_mod[i];
        for (uint32_t k = 0; k < m->nsym; k++) {
            const SymRec *s = &m->syms[k];
            if (s->defined && s->bind != 0 && s->name_off) { exp_add(m->strtab + s->name_off, m->base + s->value, s->type, (uint8_t)i); m->n_defined++; }
        }
    }
    rt_log("export table: %u global definitions (main %u, sdk %u, subsdk0 %u)", g_nexp, g_mod[0].n_defined, g_mod[1].n_defined, g_mod[2].n_defined);
    for (int i = 0; i < NMODULES; i++) relocate(i);
    provide_rtld_vars();
    return 0;
}
