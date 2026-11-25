/* Star Road Recomp fs services (task-13): fsp-srv with a RomFS storage and host-backed save data. Our own code.
 *
 * RomFS: the sdk mounts "rom:" by opening an IStorage (fsp-srv cmd 200) and parsing the RomFS image through it. We serve a
 * virtual image: a synthesized header and hash/meta tables built from the extraction directory (hle_romfs_dir()) and the
 * file data region read straight from the host files. Nothing is copied or cached beyond the tables.
 * Save data: IFileSystem objects over a host directory under hle_save_dir() on A:.
 * Layouts (RomFS header, entry formats, DirectoryEntry) are the documented Horizon formats (switchbrew "RomFS"). */
#include "hle.h"
#include "mods/postgame.h"   /* task-24: unlock_postgame patches the save as the game reads it */
#include "mods/datatbl_overlay.h" /* task-25: bounded monster table cache */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ------------------------------------------------------------------ host path helpers */
static int utf8_to_wide(const char *s, wchar_t *w, int n) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) > 0; }
static int wide_to_utf8(const wchar_t *w, char *s, int n) { return WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, 0, 0) > 0; }

/* join root and a guest path ("/a/b"); rejects ".." and returns 0 on a bad path */
static int host_path(const char *root, const char *guest, wchar_t *out, int n) {
    char tmp[1024];
    size_t rl = strlen(root);
    if (rl + strlen(guest) + 2 >= sizeof tmp) return 0;
    memcpy(tmp, root, rl);
    char *d = tmp + rl;
    if (rl && tmp[rl - 1] != '/' && tmp[rl - 1] != '\\') *d++ = '/';
    for (const char *p = guest; *p; p++) {
        if (p == guest && (*p == '/' || *p == '\\')) continue;
        *d++ = *p == '\\' ? '/' : *p;
    }
    *d = 0;
    if (strstr(tmp + rl, "..")) return 0;
    for (char *p = tmp; *p; p++) if (*p == '/') *p = '\\';
    /* trim a trailing separator (but keep a drive root) */
    size_t l = strlen(tmp);
    while (l > 3 && tmp[l - 1] == '\\') tmp[--l] = 0;
    return utf8_to_wide(tmp, out, n);
}
static void mkdirs_w(wchar_t *w) {
    for (wchar_t *p = w + 3; *p; p++)
        if (*p == L'\\') { *p = 0; CreateDirectoryW(w, 0); *p = L'\\'; }
    CreateDirectoryW(w, 0);
}

/* ------------------------------------------------------------------ RomFS virtual image */
typedef struct RfFile { char *rel; wchar_t *wpath; uint64_t size; uint64_t data_off; int parent; uint32_t meta_off; uint32_t sibling_idx; HANDLE h; int is_level0; uint8_t *overlay; } RfFile;
typedef struct RfDir { char *name; int parent; int *dirs, ndirs; int *files, nfiles; uint32_t meta_off; } RfDir;
typedef struct RomFs {
    RfDir *dirs; int ndirs, capd;
    RfFile *files; int nfiles, capf;
    uint8_t *meta; uint64_t meta_size;        /* header + tables, from offset 0 */
    uint64_t data_start, total;
    int ready;
} RomFs;
static RomFs g_rom;
static CRITICAL_SECTION g_rom_cs;
static INIT_ONCE g_rom_once = INIT_ONCE_STATIC_INIT;
static INIT_ONCE g_monster_overlay_once = INIT_ONCE_STATIC_INIT;
#define MONSTER_OVERLAY_CACHE_MAX (16u * 1024u * 1024u)
static BOOL CALLBACK rom_once_cb(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&g_rom_cs); return TRUE; }

static int cmp_dir_idx(const void *a, const void *b);
static RomFs *g_sort_ctx;
static int cmp_dir_idx(const void *a, const void *b) { return strcmp(g_sort_ctx->dirs[*(const int *)a].name, g_sort_ctx->dirs[*(const int *)b].name); }
static int cmp_file_idx(const void *a, const void *b) { return strcmp(g_sort_ctx->files[*(const int *)a].rel + 0, g_sort_ctx->files[*(const int *)b].rel + 0); }

static int rf_add_dir(RomFs *r, const char *name, int parent) {
    if (r->ndirs == r->capd) { r->capd = r->capd ? r->capd * 2 : 64; r->dirs = realloc(r->dirs, r->capd * sizeof *r->dirs); }
    RfDir *d = &r->dirs[r->ndirs];
    memset(d, 0, sizeof *d);
    d->name = _strdup(name);
    d->parent = parent;
    return r->ndirs++;
}
static int rf_add_file(RomFs *r, const char *rel, const char *name, const wchar_t *wpath, uint64_t size, int parent) {
    if (r->nfiles == r->capf) { r->capf = r->capf ? r->capf * 2 : 256; r->files = realloc(r->files, r->capf * sizeof *r->files); }
    RfFile *f = &r->files[r->nfiles];
    memset(f, 0, sizeof *f);
    (void)name;
    f->rel = _strdup(rel);
    f->wpath = _wcsdup(wpath);
    f->size = size;
    f->parent = parent;
    f->h = INVALID_HANDLE_VALUE;
    f->is_level0 = strcmp(rel, "Data/level0") == 0;
    return r->nfiles++;
}
static void rf_scan(RomFs *r, const wchar_t *wdir, const char *rel, int dir_idx) {
    wchar_t pat[1024];
    swprintf(pat, 1024, L"%ls\\*", wdir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char name[512];
        wide_to_utf8(fd.cFileName, name, sizeof name);
        char crel[1024];
        snprintf(crel, sizeof crel, "%s%s%s", rel, rel[0] ? "/" : "", name);
        wchar_t wp[1024];
        swprintf(wp, 1024, L"%ls\\%ls", wdir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            int di = rf_add_dir(r, name, dir_idx);
            rf_scan(r, wp, crel, di);
        } else {
            uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            int fi = rf_add_file(r, crel, name, wp, sz, dir_idx);
            (void)fi;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static uint32_t rf_hash(uint32_t parent, const char *s, uint32_t n) {
    uint32_t h = parent ^ 123456789u;
    for (uint32_t i = 0; i < n; i++) { h = (h >> 5) | (h << 27); h ^= (uint8_t)s[i]; }
    return h;
}
static uint32_t rf_buckets(uint32_t n) { uint32_t c = n < 3 ? 3 : (n | 1); while (c % 3 == 0 || c % 5 == 0 || c % 7 == 0) c += 2; return c; }
static const char *base_name(const char *rel) { const char *s = strrchr(rel, '/'); return s ? s + 1 : rel; }
static uint32_t al4(uint32_t v) { return (v + 3) & ~3u; }

static void rf_build(RomFs *r) {
    wchar_t wroot[1024];
    utf8_to_wide(hle_romfs_dir(), wroot, 1024);
    rf_add_dir(r, "", 0);
    rf_scan(r, wroot, "", 0);
    /* children lists, sorted by name */
    g_sort_ctx = r;
    for (int i = 0; i < r->ndirs; i++) { r->dirs[i].dirs = malloc(sizeof(int) * (r->ndirs ? 1 : 1)); }
    int *cnt = calloc(r->ndirs, sizeof(int));
    for (int i = 1; i < r->ndirs; i++) cnt[r->dirs[i].parent]++;
    for (int i = 0; i < r->ndirs; i++) { free(r->dirs[i].dirs); r->dirs[i].dirs = malloc(sizeof(int) * (cnt[i] + 1)); r->dirs[i].ndirs = 0; }
    for (int i = 1; i < r->ndirs; i++) { RfDir *p = &r->dirs[r->dirs[i].parent]; p->dirs[p->ndirs++] = i; }
    memset(cnt, 0, r->ndirs * sizeof(int));
    for (int i = 0; i < r->nfiles; i++) cnt[r->files[i].parent]++;
    for (int i = 0; i < r->ndirs; i++) { r->dirs[i].files = malloc(sizeof(int) * (cnt[i] + 1)); r->dirs[i].nfiles = 0; }
    for (int i = 0; i < r->nfiles; i++) { RfDir *p = &r->dirs[r->files[i].parent]; p->files[p->nfiles++] = i; }
    free(cnt);
    for (int i = 0; i < r->ndirs; i++) {
        qsort(r->dirs[i].dirs, r->dirs[i].ndirs, sizeof(int), cmp_dir_idx);
        qsort(r->dirs[i].files, r->dirs[i].nfiles, sizeof(int), cmp_file_idx);
    }
    /* meta offsets */
    uint32_t doff = 0, foff = 0;
    for (int i = 0; i < r->ndirs; i++) { r->dirs[i].meta_off = doff; doff += 0x18 + al4((uint32_t)strlen(r->dirs[i].name)); }
    for (int i = 0; i < r->nfiles; i++) { r->files[i].meta_off = foff; foff += 0x20 + al4((uint32_t)strlen(base_name(r->files[i].rel))); }
    uint32_t dbuckets = rf_buckets((uint32_t)r->ndirs), fbuckets = rf_buckets((uint32_t)r->nfiles);
    uint64_t dht = 0x200, dmeta = dht + 4ull * dbuckets, fht = (dmeta + doff + 3) & ~3ull, fmeta = fht + 4ull * fbuckets;
    r->meta_size = (fmeta + foff + 0xFFF) & ~0xFFFull;
    r->meta = calloc(1, r->meta_size);
    uint8_t *m = r->meta;
    uint32_t *dh = (uint32_t *)(m + dht), *fh = (uint32_t *)(m + fht);
    for (uint32_t i = 0; i < dbuckets; i++) dh[i] = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < fbuckets; i++) fh[i] = 0xFFFFFFFFu;
    /* file data layout */
    uint64_t pos = r->meta_size;
    r->data_start = pos;
    for (int i = 0; i < r->nfiles; i++) {
        r->files[i].data_off = pos - r->data_start;
        pos += (r->files[i].size + 0xF) & ~0xFull;
    }
    r->total = pos;
    /* directory entries */
    for (int i = 0; i < r->ndirs; i++) {
        RfDir *d = &r->dirs[i];
        uint32_t *e = (uint32_t *)(m + dmeta + d->meta_off);
        uint32_t nl = (uint32_t)strlen(d->name);
        uint32_t parent_off = i == 0 ? 0 : r->dirs[d->parent].meta_off;
        uint32_t sibling = 0xFFFFFFFFu;
        if (i) {
            RfDir *p = &r->dirs[d->parent];
            for (int k = 0; k < p->ndirs; k++) if (p->dirs[k] == i) { if (k + 1 < p->ndirs) sibling = r->dirs[p->dirs[k + 1]].meta_off; break; }
        }
        e[0] = parent_off;
        e[1] = sibling;
        e[2] = d->ndirs ? r->dirs[d->dirs[0]].meta_off : 0xFFFFFFFFu;
        e[3] = d->nfiles ? r->files[d->files[0]].meta_off : 0xFFFFFFFFu;
        uint32_t hb = rf_hash(parent_off, d->name, nl) % dbuckets;
        e[4] = dh[hb];
        dh[hb] = d->meta_off;
        e[5] = nl;
        memcpy(e + 6, d->name, nl);
    }
    for (int i = 0; i < r->ndirs; i++) {
        RfDir *d = &r->dirs[i];
        for (int k = 0; k < d->nfiles; k++) {
            RfFile *f = &r->files[d->files[k]];
            const char *nm = base_name(f->rel);
            uint32_t nl = (uint32_t)strlen(nm);
            uint8_t *e = m + fmeta + f->meta_off;
            uint32_t parent_off = d->meta_off;
            uint32_t sibling = k + 1 < d->nfiles ? r->files[d->files[k + 1]].meta_off : 0xFFFFFFFFu;
            memcpy(e + 0, &parent_off, 4);
            memcpy(e + 4, &sibling, 4);
            memcpy(e + 8, &f->data_off, 8);
            memcpy(e + 16, &f->size, 8);
            uint32_t hb = rf_hash(parent_off, nm, nl) % fbuckets;
            memcpy(e + 24, &fh[hb], 4);
            fh[hb] = f->meta_off;
            memcpy(e + 28, &nl, 4);
            memcpy(e + 32, nm, nl);
        }
    }
    /* header */
    uint64_t *hd = (uint64_t *)m;
    hd[0] = 0x50;
    hd[1] = dht; hd[2] = 4ull * dbuckets;
    hd[3] = dmeta; hd[4] = doff;
    hd[5] = fht; hd[6] = 4ull * fbuckets;
    hd[7] = fmeta; hd[8] = foff;
    hd[9] = r->data_start;
    r->ready = 1;
    rt_log("hle fs: RomFS virtual image from %s: %d dirs, %d files, %.2f GB, tables %llu bytes", hle_romfs_dir(), r->ndirs, r->nfiles,
           r->total / 1073741824.0, (unsigned long long)r->meta_size);
}
static RomFs *rom_get(void) {
    InitOnceExecuteOnce(&g_rom_once, rom_once_cb, 0, 0);
    EnterCriticalSection(&g_rom_cs);
    if (!g_rom.ready) rf_build(&g_rom);
    LeaveCriticalSection(&g_rom_cs);
    return &g_rom;
}
static BOOL CALLBACK monster_overlay_once_cb(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once; (void)context;
    RfFile *f = (RfFile *)parameter;
    DatatblSettings settings;
    DatatblReport report;
    uint8_t *source = NULL, *overlay = NULL;
    size_t overlay_size = 0;
    HANDLE h = INVALID_HANDLE_VALUE;
    size_t got = 0;

    datatbl_settings_load_options(&settings);
    if (!settings.hard) return TRUE;
    if (!f->size || f->size > MONSTER_OVERLAY_CACHE_MAX) {
        rt_log("hle fs: difficulty_plus skipped monster.tbl cache, size %llu exceeds limit %u", (unsigned long long)f->size, MONSTER_OVERLAY_CACHE_MAX);
        return TRUE;
    }
    source = (uint8_t *)malloc((size_t)f->size);
    if (!source) goto fallback;
    h = CreateFileW(f->wpath, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) goto fallback;
    while (got < (size_t)f->size) {
        DWORD want = (DWORD)(((size_t)f->size - got) > 0x100000 ? 0x100000 : (size_t)f->size - got);
        DWORD rd = 0;
        if (!ReadFile(h, source + got, want, &rd, 0) || !rd) goto fallback;
        got += rd;
    }
    CloseHandle(h);
    h = INVALID_HANDLE_VALUE;
    if (!datatbl_overlay_prepare_cache(source, (size_t)f->size, &settings, MONSTER_OVERLAY_CACHE_MAX,
                                       &overlay, &overlay_size, &report)) goto fallback;
    free(source);
    f->overlay = overlay;
    rt_log("hle fs: difficulty_plus serving modified monster.tbl (%llu bytes, %llu monsters, %llu stats changed, %llu saturated)",
           (unsigned long long)overlay_size, (unsigned long long)report.monster_count,
           (unsigned long long)report.changed_stats, (unsigned long long)report.saturated_stats);
    return TRUE;

fallback:
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    free(source);
    free(overlay);
    rt_log("hle fs: difficulty_plus could not prepare monster.tbl; serving original bytes");
    return TRUE;
}
/* read len bytes at off of the virtual image into dst; returns bytes produced (the image end clamps) */
static uint64_t rom_read(RomFs *r, uint64_t off, void *dst, uint64_t len) {
    if (off >= r->total) return 0;
    if (off + len > r->total) len = r->total - off;
    uint8_t *o = dst;
    uint64_t done = 0;
    while (done < len) {
        uint64_t p = off + done;
        if (p < r->meta_size) {
            uint64_t n = r->meta_size - p;
            if (n > len - done) n = len - done;
            memcpy(o + done, r->meta + p, n);
            done += n;
            continue;
        }
        uint64_t rel = p - r->data_start;
        int lo = 0, hi = r->nfiles - 1, idx = -1;
        while (lo <= hi) {      /* last file with data_off <= rel (files are laid out in index order) */
            int mid = (lo + hi) / 2;
            if (r->files[mid].data_off <= rel) { idx = mid; lo = mid + 1; } else hi = mid - 1;
        }
        if (idx < 0) { memset(o + done, 0, len - done); break; }
        RfFile *f = &r->files[idx];
        uint64_t fo = rel - f->data_off;
        uint64_t slot_end = ((f->size + 0xF) & ~0xFull);
        uint64_t n = slot_end - fo;
        if (n > len - done) n = len - done;
        uint64_t real = fo < f->size ? (f->size - fo < n ? f->size - fo : n) : 0;
        if (real) {
            if (f->is_level0 && !g_level0_reached) hle_boot_marker("level0 read (first read of Data/level0)");
            if (_stricmp(f->rel, "Data/StreamingAssets/data_tbl/monster.tbl") == 0) {
                InitOnceExecuteOnce(&g_monster_overlay_once, monster_overlay_once_cb, f, 0);
            }
            if (f->overlay) memcpy(o + done, f->overlay + fo, (size_t)real);
            else {
                if (f->h == INVALID_HANDLE_VALUE) {
                    HANDLE h = CreateFileW(f->wpath, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
                    HANDLE prev = InterlockedCompareExchangePointer(&f->h, h, INVALID_HANDLE_VALUE);
                    if (prev != INVALID_HANDLE_VALUE) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
                }
                if (f->h == INVALID_HANDLE_VALUE) memset(o + done, 0, real);
                else {
                    uint64_t got = 0;
                    while (got < real) {
                        OVERLAPPED ov;
                        memset(&ov, 0, sizeof ov);
                        uint64_t at = fo + got;
                        ov.Offset = (DWORD)at;
                        ov.OffsetHigh = (DWORD)(at >> 32);
                        DWORD want = (DWORD)(real - got > 0x4000000 ? 0x4000000 : real - got), rd = 0;
                        if (!ReadFile(f->h, o + done + got, want, &rd, &ov) || !rd) { memset(o + done + got, 0, real - got); break; }
                        got += rd;
                    }
                }
            }
        }
        if (n > real) memset(o + done + real, 0, n - real);
        done += n;
    }
    return done;
}

/* IStorage for the RomFS */
static void storage_read(IpcCtx *x) {
    uint64_t off = in_u64(x, 0), len = in_u64(x, 8);
    IpcBuf *b = ipc_out_buf(x, 0);
    if (!b) { x->result = RES_FS_INVALID_ARG; return; }
    RomFs *r = rom_get();
    if (off > r->total || off + len > r->total) { x->result = RES_FS_OUT_OF_RANGE; return; }
    if (len > b->size) len = b->size;
    rom_read(r, off, (void *)b->addr, len);
}
static void storage_get_size(IpcCtx *x) { out_u64(x, 0, rom_get()->total); }
static void storage_flush(IpcCtx *x) { (void)x; }
CMD_TABLE(g_storage_cmds) = {CMD(0, storage_read), CMD(3, storage_flush), CMD(4, storage_get_size)};
IFACE(g_storage_iface, "nn::fssrv::sf::IStorage", g_storage_cmds);

/* ------------------------------------------------------------------ host backed IFileSystem / IFile / IDirectory */
typedef struct HostFs { char root[512]; } HostFs;
typedef struct HostFile { HANDLE h; int writable; uint8_t *mem; size_t mem_n; /* task-24: patched save image served instead of the host file */ } HostFile;
typedef struct HostDir { wchar_t dir[1024]; int filter; WIN32_FIND_DATAW *ents; int n, pos; } HostDir;

static void file_dtor(void *s) { HostFile *f = s; if (f) { if (f->h != INVALID_HANDLE_VALUE) CloseHandle(f->h); free(f->mem); free(f); } }
static void dir_dtor(void *s) { HostDir *d = s; if (d) { free(d->ents); free(d); } }
static void fs_dtor(void *s) { free(s); }

static HostFile *self_file(IpcCtx *x) { return x->self ? (HostFile *)x->self->state : 0; }
static void file_read(IpcCtx *x) {
    HostFile *f = self_file(x);
    uint64_t off = in_u64(x, 8), len = in_u64(x, 16);
    IpcBuf *b = ipc_out_buf(x, 0);
    if (!f || !b) { x->result = RES_FS_INVALID_ARG; return; }
    if (len > b->size) len = b->size;
    uint64_t got = 0;
    if (f->mem) {   /* task-24: patched save image */
        if (off < f->mem_n) { got = f->mem_n - off < len ? f->mem_n - off : len; memcpy((uint8_t *)b->addr, f->mem + off, (size_t)got); }
        out_u64(x, 0, got);
        return;
    }
    while (got < len) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        uint64_t at = off + got;
        ov.Offset = (DWORD)at; ov.OffsetHigh = (DWORD)(at >> 32);
        DWORD rd = 0, want = (DWORD)(len - got > 0x4000000 ? 0x4000000 : len - got);
        if (!ReadFile(f->h, (uint8_t *)b->addr + got, want, &rd, &ov) || !rd) break;
        got += rd;
    }
    out_u64(x, 0, got);
}
static void file_write(IpcCtx *x) {
    HostFile *f = self_file(x);
    uint64_t off = in_u64(x, 8), len = in_u64(x, 16);
    IpcBuf *b = ipc_in_buf(x, 0);
    if (!f || !b) { x->result = RES_FS_INVALID_ARG; return; }
    if (len > b->size) len = b->size;
    uint64_t put = 0;
    while (put < len) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        uint64_t at = off + put;
        ov.Offset = (DWORD)at; ov.OffsetHigh = (DWORD)(at >> 32);
        DWORD wr = 0, want = (DWORD)(len - put > 0x4000000 ? 0x4000000 : len - put);
        if (!WriteFile(f->h, (const uint8_t *)b->addr + put, want, &wr, &ov) || !wr) { x->result = RES_FS_INVALID_ARG; return; }
        put += wr;
    }
}
static void file_flush(IpcCtx *x) { HostFile *f = self_file(x); if (f) FlushFileBuffers(f->h); }
static void file_set_size(IpcCtx *x) {
    HostFile *f = self_file(x);
    if (!f) return;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)in_u64(x, 0);
    if (!SetFilePointerEx(f->h, li, 0, FILE_BEGIN) || !SetEndOfFile(f->h)) x->result = RES_FS_INVALID_ARG;
}
static void file_get_size(IpcCtx *x) {
    HostFile *f = self_file(x);
    LARGE_INTEGER li = {0};
    if (f) GetFileSizeEx(f->h, &li);
    if (f && f->mem) li.QuadPart = (LONGLONG)f->mem_n;
    out_u64(x, 0, (uint64_t)li.QuadPart);
}
CMD_TABLE(g_file_cmds) = {CMD(0, file_read), CMD(1, file_write), CMD(2, file_flush), CMD(3, file_set_size), CMD(4, file_get_size)};
IFACE_D(g_file_iface, "nn::fssrv::sf::IFile", g_file_cmds, file_dtor);

/* nn::fs::DirectoryEntry: char name[0x301]; pad[3]; s8 type @0x304 (0 dir, 1 file); pad; s64 size @0x308; 0x310 bytes */
static void dir_read(IpcCtx *x) {
    HostDir *d = (HostDir *)x->self->state;
    IpcBuf *b = ipc_out_buf(x, 0);
    if (!b) { x->result = RES_FS_INVALID_ARG; return; }
    uint64_t cap = b->size / 0x310, n = 0;
    uint8_t *o = (uint8_t *)b->addr;
    while (n < cap && d->pos < d->n) {
        WIN32_FIND_DATAW *e = &d->ents[d->pos++];
        uint8_t *rec = o + n * 0x310;
        memset(rec, 0, 0x310);
        char nm[0x300];
        wide_to_utf8(e->cFileName, nm, sizeof nm);
        strncpy((char *)rec, nm, 0x300);
        int isdir = (e->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        rec[0x304] = isdir ? 0 : 1;
        uint64_t sz = isdir ? 0 : (((uint64_t)e->nFileSizeHigh << 32) | e->nFileSizeLow);
        memcpy(rec + 0x308, &sz, 8);
        n++;
    }
    out_u64(x, 0, n);
}
static void dir_count(IpcCtx *x) { HostDir *d = (HostDir *)x->self->state; out_u64(x, 0, (uint64_t)d->n); }
CMD_TABLE(g_dir_cmds) = {CMD(0, dir_read), CMD(1, dir_count)};
IFACE_D(g_dir_iface, "nn::fssrv::sf::IDirectory", g_dir_cmds, dir_dtor);

static HostFs *self_fs(IpcCtx *x) { return (HostFs *)x->self->state; }
static int path_arg(IpcCtx *x, int i, wchar_t *out, int n) {
    IpcBuf *b = ipc_in_buf(x, i);
    if (!b || !b->addr) return 0;
    char p[0x310];
    uint64_t l = b->size < sizeof p - 1 ? b->size : sizeof p - 1;
    memcpy(p, (const void *)b->addr, l);
    p[l] = 0;
    return host_path(self_fs(x)->root, p, out, n);
}
static void fsys_create_file(IpcCtx *x) {
    wchar_t w[1024];
    if (!path_arg(x, 0, w, 1024)) { x->result = RES_FS_INVALID_ARG; return; }
    uint64_t size = in_u64(x, 8);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { x->result = GetLastError() == ERROR_FILE_EXISTS ? RES_FS_PATH_EXISTS : RES_FS_PATH_NOT_FOUND; return; }
    if (size) { LARGE_INTEGER li; li.QuadPart = (LONGLONG)size; SetFilePointerEx(h, li, 0, FILE_BEGIN); SetEndOfFile(h); }
    CloseHandle(h);
}
static void fsys_delete_file(IpcCtx *x) {
    wchar_t w[1024];
    if (!path_arg(x, 0, w, 1024)) { x->result = RES_FS_INVALID_ARG; return; }
    if (!DeleteFileW(w)) x->result = RES_FS_PATH_NOT_FOUND;
}
static void fsys_create_dir(IpcCtx *x) {
    wchar_t w[1024];
    if (!path_arg(x, 0, w, 1024)) { x->result = RES_FS_INVALID_ARG; return; }
    if (!CreateDirectoryW(w, 0)) x->result = GetLastError() == ERROR_ALREADY_EXISTS ? RES_FS_PATH_EXISTS : RES_FS_PATH_NOT_FOUND;
}
static void fsys_delete_dir(IpcCtx *x) {
    wchar_t w[1024];
    if (!path_arg(x, 0, w, 1024)) { x->result = RES_FS_INVALID_ARG; return; }
    if (!RemoveDirectoryW(w)) x->result = RES_FS_PATH_NOT_FOUND;
}
static void rm_tree(wchar_t *w) {
    wchar_t pat[1024];
    swprintf(pat, 1024, L"%ls\\*", w);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t p[1024];
        swprintf(p, 1024, L"%ls\\%ls", w, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { rm_tree(p); RemoveDirectoryW(p); } else DeleteFileW(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void fsys_delete_dir_rec(IpcCtx *x) {
    wchar_t w[1024];
    if (!path_arg(x, 0, w, 1024)) { x->result = RES_FS_INVALID_ARG; return; }
    rm_tree(w);
    RemoveDirectoryW(w);
}
static void fsys_clean_dir_rec(IpcCtx *x) {
    wchar_t w[1024];
    if (!path_arg(x, 0, w, 1024)) { x->result = RES_FS_INVALID_ARG; return; }
    rm_tree(w);
}
static void fsys_rename(IpcCtx *x) {
    wchar_t a[1024], b[1024];
    if (!path_arg(x, 0, a, 1024) || !path_arg(x, 1, b, 1024)) { x->result = RES_FS_INVALID_ARG; return; }
    if (!MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING)) x->result = RES_FS_PATH_NOT_FOUND;
}
static void fsys_entry_type(IpcCtx *x) {
    wchar_t w[1024];
    if (!path_arg(x, 0, w, 1024)) { x->result = RES_FS_INVALID_ARG; return; }
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) { x->result = RES_FS_PATH_NOT_FOUND; return; }
    out_u32(x, 0, (a & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1);
}
/* task-24 unlock_postgame: a save file opened for reading is served from memory with the post-game flags set; the file on disk is not touched.
 * The option is read once (SMRPG_UNLOCK_POSTGAME, else [gameplay] unlock_postgame in SMRPG_OPTIONS or options.ini beside the exe). Off: no read, no copy. */
static void pg_log(void *ud, const char *line) { (void)ud; rt_log("%s", line); }
static int pg_enabled(void) {
    static int state = -1;
    if (state < 0) {
        char ini[MAX_PATH] = "";
        const char *e = getenv("SMRPG_OPTIONS");
        if (e && *e) snprintf(ini, sizeof ini, "%s", e);
        else if (GetModuleFileNameA(0, ini, sizeof ini)) { char *s = strrchr(ini, 92); if (s) strcpy(s + 1, "options.ini"); else ini[0] = 0; }
        state = postgame_enabled(ini);
        if (state) rt_log("hle fs: unlock_postgame is ON");
    }
    return state;
}
static void save_unlock_view(HostFile *f, const wchar_t *wpath) {
    if (!pg_enabled()) return;
    const wchar_t *base = wcsrchr(wpath, 92), *base2 = wcsrchr(wpath, L'/');
    if (!base || (base2 && base2 > base)) base = base2;
    char name[260];
    if (!wide_to_utf8(base ? base + 1 : wpath, name, sizeof name) || !postgame_is_save_name(name)) return;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f->h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64 << 20)) return;
    uint8_t *in = malloc((size_t)sz.QuadPart);
    DWORD rd = 0;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    if (!in || !ReadFile(f->h, in, (DWORD)sz.QuadPart, &rd, &ov) || rd != (DWORD)sz.QuadPart) { free(in); return; }
    size_t n = 0;
    uint8_t *out = postgame_filter_save(1, in, rd, &n, pg_log, 0);
    free(in);
    if (out) { f->mem = out; f->mem_n = n; rt_log("hle fs: save %s served with the post-game flags set (%zu bytes)", name, n); }
}
static void fsys_open_file(IpcCtx *x) {
    wchar_t w[1024];
    if (!path_arg(x, 0, w, 1024)) { x->result = RES_FS_INVALID_ARG; return; }
    uint32_t mode = in_u32(x, 0);
    int wr = (mode & 6) != 0;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) { x->result = RES_FS_PATH_NOT_FOUND; return; }
    HANDLE h = CreateFileW(w, wr ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { x->result = RES_FS_TARGET_LOCKED; return; }
    HostFile *f = calloc(1, sizeof *f);
    f->h = h;
    f->writable = wr;
    if (!wr) save_unlock_view(f, w);
    out_obj(x, &g_file_iface, f);
}
static void fsys_open_dir(IpcCtx *x) {
    wchar_t w[1024];
    if (!path_arg(x, 0, w, 1024)) { x->result = RES_FS_INVALID_ARG; return; }
    uint32_t filter = in_u32(x, 0);
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { x->result = RES_FS_PATH_NOT_FOUND; return; }
    HostDir *d = calloc(1, sizeof *d);
    wcsncpy(d->dir, w, 1023);
    d->filter = (int)filter;
    int cap = 0;
    wchar_t pat[1024];
    swprintf(pat, 1024, L"%ls\\*", w);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if ((isdir && !(filter & 1)) || (!isdir && !(filter & 2))) continue;
            if (d->n == cap) { cap = cap ? cap * 2 : 16; d->ents = realloc(d->ents, cap * sizeof *d->ents); }
            d->ents[d->n++] = fd;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    out_obj(x, &g_dir_iface, d);
}
static void fsys_commit(IpcCtx *x) { (void)x; }
static void fsys_free_space(IpcCtx *x) { out_u64(x, 0, 0x100000000ull); }
static void fsys_total_space(IpcCtx *x) { out_u64(x, 0, 0x200000000ull); }
static void fsys_timestamp(IpcCtx *x) {
    wchar_t w[1024];
    if (!path_arg(x, 0, w, 1024)) { x->result = RES_FS_INVALID_ARG; return; }
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &d)) { x->result = RES_FS_PATH_NOT_FOUND; return; }
    uint64_t t[3];
    t[0] = (((uint64_t)d.ftCreationTime.dwHighDateTime << 32) | d.ftCreationTime.dwLowDateTime) / 10000000ull - 11644473600ull;
    t[1] = (((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime) / 10000000ull - 11644473600ull;
    t[2] = (((uint64_t)d.ftLastAccessTime.dwHighDateTime << 32) | d.ftLastAccessTime.dwLowDateTime) / 10000000ull - 11644473600ull;
    out_bytes(x, 0, t, 24);
    out_u32(x, 24, 1);
}
static void fsys_attribute(IpcCtx *x) { out_zero(x, 0, 0x40); }
CMD_TABLE(g_fsys_cmds) = {
    CMD(0, fsys_create_file), CMD(1, fsys_delete_file), CMD(2, fsys_create_dir), CMD(3, fsys_delete_dir), CMD(4, fsys_delete_dir_rec),
    CMD(5, fsys_rename), CMD(6, fsys_rename), CMD(7, fsys_entry_type), CMD(8, fsys_open_file), CMD(9, fsys_open_dir), CMD(10, fsys_commit),
    CMD(11, fsys_free_space), CMD(12, fsys_total_space), CMD(13, fsys_clean_dir_rec), CMD(14, fsys_timestamp), CMD(16, fsys_attribute)};
IFACE_D(g_fsys_iface, "nn::fssrv::sf::IFileSystem", g_fsys_cmds, fs_dtor);

/* ------------------------------------------------------------------ fsp-srv */
static void fsp_set_current_process(IpcCtx *x) { (void)x; }
static void fsp_open_data_storage(IpcCtx *x) { rom_get(); out_obj(x, &g_storage_iface, 0); }
static void fsp_nop(IpcCtx *x) { (void)x; }
static void fsp_u32_0(IpcCtx *x) { out_u32(x, 0, 0); }
static void fsp_u8_0(IpcCtx *x) { out_bytes(x, 0, &(uint8_t){0}, 1); }

/* SaveDataAttribute (0x40): program_id u64, user_id u128 @8, system_save_data_id u64 @0x18, type u8 @0x20, rank u8, index u16 */
static void save_dir_for(IpcCtx *x, uint32_t attr_off, char *out, size_t n) {
    uint64_t u0 = in_u64(x, attr_off + 8), u1 = in_u64(x, attr_off + 16);
    uint8_t type = x->in_size > attr_off + 0x20 ? x->in[attr_off + 0x20] : 0;
    uint64_t sys = in_u64(x, attr_off + 0x18);
    snprintf(out, n, "%s/user_%016llx%016llx_t%u_s%llx", hle_save_dir(), (unsigned long long)u1, (unsigned long long)u0, type, (unsigned long long)sys);
}
static void fsp_open_save(IpcCtx *x) {
    HostFs *f = calloc(1, sizeof *f);
    save_dir_for(x, 8, f->root, sizeof f->root);     /* payload: u8 space (padded), attribute @8 */
    wchar_t w[1024];
    utf8_to_wide(f->root, w, 1024);
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    mkdirs_w(w);
    out_obj(x, &g_fsys_iface, f);
}
static void fsp_create_save(IpcCtx *x) {
    char dir[512];
    save_dir_for(x, 0, dir, sizeof dir);
    wchar_t w[1024];
    utf8_to_wide(dir, w, 1024);
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    mkdirs_w(w);
}
CMD_TABLE(g_fsp_cmds) = {
    CMD(1, fsp_set_current_process), CMD(22, fsp_create_save), CMD(27, fsp_u8_0), CMD(51, fsp_open_save), CMD(53, fsp_open_save),
    CMD(200, fsp_open_data_storage), CMD(1003, fsp_nop), CMD(1004, fsp_nop), CMD(1005, fsp_u32_0), CMD(1006, fsp_nop), CMD(1016, fsp_nop)};
IFACE(g_fsp_iface, "nn::fssrv::sf::IFileSystemProxy", g_fsp_cmds);
static const ServiceDef g_fsp_def = {"fsp-srv", &g_fsp_iface, 0};

void hle_fs_init(void) {
    InitOnceExecuteOnce(&g_rom_once, rom_once_cb, 0, 0);
    hle_register_service(&g_fsp_def);
}
