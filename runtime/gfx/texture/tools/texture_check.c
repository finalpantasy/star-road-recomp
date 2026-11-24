/* Star Road Recomp offline texture checker (task-11). Our own code. Windows x64, clang.
 *
 *   texture_check survey    [opts]   AC1: format/layout census; plan vs blob offsets for every Texture2D
 *   texture_check roundtrip [opts]   AC3: deswizzle -> swizzle on real data, valid bytes must match the original
 *   texture_check astc      [opts]   AC2: our ASTC decode vs ARM astcenc (test oracle, run as a separate process)
 *   texture_check bc7       [opts]   ASTC -> BC7 transcode quality (PSNR) on a sample
 *   texture_check dump --ids a,b,c   write PNGs of every mip of chosen manifest ids
 *
 * Opts: --manifest F  --aa DIR  --out DIR  --report FILE  --limit N  --every K  --first ID  --last ID
 *       --shard I/N  --threads T  --oracle EXE  --fmt U  (Unity format id filter)
 * Reads the owner's bundles read-only; writes only under --out (default A:\AI\Recomps\smrpg-work\textures). */
#define _CRT_SECURE_NO_WARNINGS
#include "texture.h"
#include "tex_unityfs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#endif

int png_write_rgba(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h);

typedef struct {
    int id; char* bundle; char* name; int fmt; uint32_t w, h, mips; int cs; uint64_t complete, resoff, stoff, stsize;
    char* inline_file; uint8_t blob[92]; size_t blob_len; int image_count, dim, stripped;
} row_t;

static row_t* g_rows; static size_t g_nrows;
static const char* g_aa = "A:\\AI\\Recomps\\smrpg-work\\v101\\romfs\\Data\\StreamingAssets\\aa\\Switch";
static const char* g_manifest = "A:\\AI\\Recomps\\smrpg-work\\textures\\manifest.tsv";
static const char* g_out = "A:\\AI\\Recomps\\smrpg-work\\textures";
static const char* g_oracle = "A:\\AI\\Recomps\\tools-ext\\astcenc-5.7.1\\x\\bin\\astcenc-avx2.exe";
static const char* g_report = NULL;
static long g_limit = 0, g_every = 1, g_first = 0, g_last = 1 << 30, g_fmt = -1;
static int g_shard_i = 0, g_shard_n = 1, g_threads = 4;
static const char* g_ids = NULL;
static FILE* g_rep;

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

static int load_manifest(void) {
    FILE* f = fopen(g_manifest, "rb");
    if (!f) { fprintf(stderr, "cannot open manifest %s\n", g_manifest); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) return -1;
    buf[n] = 0; fclose(f);
    size_t cap = 16384; g_rows = (row_t*)calloc(cap, sizeof(row_t));
    char* p = strchr(buf, '\n'); if (!p) return -1; p++;
    while (*p) {
        char* e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        *e = 0;
        char* fld[20]; int nf = 0; char* q = p;
        while (nf < 20) { fld[nf++] = q; char* t = strchr(q, '\t'); if (!t) break; *t = 0; q = t + 1; }
        if (nf >= 17) {
            if (g_nrows == cap) { cap *= 2; g_rows = (row_t*)realloc(g_rows, cap * sizeof(row_t)); }
            row_t* r = &g_rows[g_nrows++];
            r->id = atoi(fld[0]); r->bundle = fld[1]; r->name = fld[2]; r->fmt = atoi(fld[3]); r->w = (uint32_t)atoi(fld[4]); r->h = (uint32_t)atoi(fld[5]);
            r->mips = (uint32_t)atoi(fld[6]); r->cs = atoi(fld[7]); r->complete = strtoull(fld[8], 0, 10);
            r->resoff = (uint64_t)strtoll(fld[9], 0, 10); r->stoff = strtoull(fld[10], 0, 10); r->stsize = strtoull(fld[11], 0, 10);
            r->inline_file = fld[12]; size_t hl = strlen(fld[13]); r->blob_len = hl / 2 > 92 ? 92 : hl / 2;
            for (size_t i = 0; i < r->blob_len; i++) r->blob[i] = (uint8_t)(hexv(fld[13][2*i]) << 4 | hexv(fld[13][2*i+1]));
            r->image_count = atoi(fld[14]); r->dim = atoi(fld[15]); r->stripped = atoi(fld[16]);
        }
        p = e + 1;
    }
    return 0;
}

static int selected(const row_t* r, size_t idx) {
    if (r->id < g_first || r->id > g_last) return 0;
    if (g_fmt >= 0 && r->fmt != g_fmt) return 0;
    if (g_every > 1 && (idx % (size_t)g_every)) return 0;
    if (g_shard_n > 1 && (int)(r->id % g_shard_n) != g_shard_i) return 0;
    return 1;
}

/* bundle cache */
static ufs g_ufs; static char g_ufs_name[512];
static int open_bundle(const char* name) {
    if (strcmp(name, g_ufs_name) == 0) return 0;
    if (g_ufs.f) ufs_close(&g_ufs);
    char path[1024]; snprintf(path, sizeof path, "%s\\%s", g_aa, name);
    for (char* c = path; *c; c++) if (*c == '/') *c = '\\';
    int r = ufs_open(&g_ufs, path);
    if (r) { g_ufs_name[0] = 0; return r; }
    snprintf(g_ufs_name, sizeof g_ufs_name, "%s", name);
    return 0;
}

/* read a texture's stored bytes; returns malloc'd buffer or NULL */
static uint8_t* read_tex(const row_t* r, size_t* size_out) {
    if (r->inline_file[0]) {
        FILE* f = fopen(r->inline_file, "rb"); if (!f) return NULL;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t* b = (uint8_t*)malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
        fclose(f); *size_out = (size_t)n; return b;
    }
    if (!r->stsize) return NULL;
    if (open_bundle(r->bundle)) return NULL;
    uint8_t* b = (uint8_t*)malloc((size_t)r->stsize + 1);
    if (ufs_read(&g_ufs, r->resoff + r->stoff, r->stsize, b)) { free(b); return NULL; }
    *size_out = (size_t)r->stsize; return b;
}

static void below_normal(void) {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif
}

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void say(const char* fmt, ...) {
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    if (g_rep) { va_start(a, fmt); vfprintf(g_rep, fmt, a); va_end(a); }
}

/* ---- survey -------------------------------------------------------------------------------------------- */

static int cmd_survey(void) {
    size_t n = 0, ok = 0, nodata = 0, unhandled = 0, mism = 0, stream_ne = 0, inl = 0, lin_ok = 0;
    struct { int fmt; size_t count, ok, mism, nodata; uint64_t bytes; } agg[64]; int nagg = 0;
    uint32_t seen_w3[8]; int nseen = 0;
    size_t bh_hist[8] = {0};
    size_t dim_nonzero2 = 0, imgc_not1 = 0, stripped_nz = 0, mip_over = 0;
    for (size_t i = 0; i < g_nrows; i++) {
        row_t* r = &g_rows[i]; if (!selected(r, i)) continue;
        n++;
        int ai = -1; for (int k = 0; k < nagg; k++) if (agg[k].fmt == r->fmt) ai = k;
        if (ai < 0) { ai = nagg++; memset(&agg[ai], 0, sizeof agg[ai]); agg[ai].fmt = r->fmt; }
        agg[ai].count++; agg[ai].bytes += r->stsize ? r->stsize : 0;
        if (r->dim != 2) dim_nonzero2++;
        if (r->image_count != 1) imgc_not1++;
        if (r->stripped) stripped_nz++;
        if (r->inline_file[0]) inl++;
        sr_tex_format f;
        if (sr_tex_format_from_unity(r->fmt, r->cs, &f)) { unhandled++; say("UNHANDLED_FORMAT id=%d fmt=%d name=%s\n", r->id, r->fmt, r->name); continue; }
        if (r->stsize == 0 && !r->inline_file[0]) { nodata++; agg[ai].nodata++; say("NODATA id=%d fmt=%d %ux%u mips=%u imgcount=%d name=%s\n", r->id, r->fmt, r->w, r->h, r->mips, r->image_count, r->name); continue; }
        if (r->blob_len >= 12) {
            uint32_t w3 = (uint32_t)(r->blob[12] | r->blob[13] << 8 | r->blob[14] << 16 | (uint32_t)r->blob[15] << 24);
            int have = 0; for (int k = 0; k < nseen; k++) if (seen_w3[k] == w3) have = 1;
            if (!have && nseen < 8) seen_w3[nseen++] = w3;
            uint32_t bh = (uint32_t)(r->blob[8] | r->blob[9] << 8); if (bh < 8) bh_hist[bh]++;
        }
        if (r->mips > SR_TEX_MAX_LEVELS) mip_over++;
        sr_tex_plan plan;
        size_t ssz = 0;
        uint64_t stored = r->inline_file[0] ? 0 : r->stsize;
        int st = sr_tex_plan_from_unity_blob_ex(r->fmt, r->cs, r->w, r->h, r->mips, r->image_count > 0 ? (uint32_t)r->image_count : 1, r->blob, r->blob_len, r->complete, &plan);
        if (st == 0 && stored && plan.total_size != stored) { st = SR_TEX_E_MISMATCH; stream_ne++; }
        (void)ssz;
        if (st == 0) { ok++; agg[ai].ok++; if (plan.stored_linear) lin_ok++; }
        else {
            mism++; agg[ai].mism++;
            if (mism <= 40) say("MISMATCH id=%d st=%d fmt=%d %ux%u mips=%u complete=%llu stream=%llu plan_total=%llu bhlog2=%u name=%s\n", r->id, st, r->fmt, r->w, r->h, r->mips,
                (unsigned long long)r->complete, (unsigned long long)r->stsize, (unsigned long long)plan.total_size, (unsigned)(r->blob[8] | r->blob[9] << 8), r->name);
        }
    }
    say("== survey: %zu textures selected\n", n);
    say("layout model matches blob offsets, total and stored size: %zu (of which empty-blob tightly packed linear: %zu)\n", ok, lin_ok);
    say("mismatch: %zu (of which stored-size differs: %zu)  no data: %zu  unhandled format: %zu  inline: %zu\n", mism, stream_ne, nodata, unhandled, inl);
    say("non-2D dimension: %zu  imageCount != 1: %zu  mipsStripped != 0: %zu  mips > %d: %zu\n", dim_nonzero2, imgc_not1, stripped_nz, SR_TEX_MAX_LEVELS, mip_over);
    say("blob word3 variants:"); for (int k = 0; k < nseen; k++) say(" 0x%x", seen_w3[k]); say("\n");
    say("block-height log2 histogram (blob word2):"); for (int k = 0; k < 8; k++) say(" %d:%zu", k, bh_hist[k]); say("\n");
    say("per Unity format: id count ok mismatch nodata stored_bytes\n");
    for (int k = 0; k < nagg; k++) {
        sr_tex_format f; const char* nm = "?";
        if (!sr_tex_format_from_unity(agg[k].fmt, 0, &f)) nm = sr_tex_format_info_get(f.fmt)->name;
        say("  %3d %-12s %6zu %6zu %5zu %5zu %llu\n", agg[k].fmt, nm, agg[k].count, agg[k].ok, agg[k].mism, agg[k].nodata, (unsigned long long)agg[k].bytes);
    }
    return (mism || unhandled) ? 1 : 0;
}

/* ---- roundtrip ----------------------------------------------------------------------------------------- */

static int cmd_roundtrip(void) {
    size_t n = 0, pass = 0, fail = 0, skipped = 0, pad_nonzero_tex = 0;
    uint64_t bytes = 0, valid = 0, pad = 0, pad_nz = 0;
    for (size_t i = 0; i < g_nrows; i++) {
        row_t* r = &g_rows[i]; if (!selected(r, i)) continue;
        sr_tex_plan plan;
        int st = sr_tex_plan_from_unity_blob_ex(r->fmt, r->cs, r->w, r->h, r->mips, r->image_count > 0 ? (uint32_t)r->image_count : 1, r->blob, r->blob_len, r->complete, &plan);
        size_t sz = 0; uint8_t* S = (r->stsize || r->inline_file[0]) ? read_tex(r, &sz) : NULL;
        if (!S || (st && st != SR_TEX_E_MISMATCH)) { skipped++; free(S); continue; }
        n++;
        const sr_tex_format_info* fi = sr_tex_format_info_get(plan.desc.format.fmt);
        (void)fi;
        uint8_t* S2 = (uint8_t*)calloc(1, sz + 1), *M = (uint8_t*)calloc(1, sz + 1);
        uint8_t* L = (uint8_t*)malloc((size_t)plan.linear_layer_stride + 1), *F = (uint8_t*)malloc((size_t)plan.linear_layer_stride + 1);
        memset(F, 0xFF, (size_t)plan.linear_layer_stride);
        int bad = 0; uint64_t tex_pad_nz = 0;
        for (uint32_t lay = 0; lay < plan.desc.layers && !bad; lay++)
            for (uint32_t m = 0; m < plan.desc.levels && !bad; m++) {
                const sr_tex_level_plan* l = &plan.level[m];
                if (sr_tex_deswizzle_level(&plan, lay, m, S, sz, L + l->linear_offset, 0, 0)) { bad = 1; break; }
                if (sr_tex_swizzle_level(&plan, lay, m, L + l->linear_offset, 0, 0, S2, sz)) { bad = 1; break; }
                if (sr_tex_swizzle_level(&plan, lay, m, F + l->linear_offset, 0, 0, M, sz)) { bad = 1; break; }
            }
        if (!bad) {
            for (size_t k = 0; k < sz; k++) {
                if ((S[k] & M[k]) != S2[k]) { bad = 2; break; }
                if (M[k]) valid++; else { pad++; if (S[k]) { pad_nz++; tex_pad_nz++; } }
            }
        }
        bytes += sz;
        if (tex_pad_nz) pad_nonzero_tex++;
        if (bad) { fail++; if (fail <= 20) say("ROUNDTRIP_FAIL id=%d fmt=%d %ux%u mips=%u why=%d name=%s\n", r->id, r->fmt, r->w, r->h, r->mips, bad, r->name); }
        else pass++;
        free(S); free(S2); free(M); free(L); free(F);
    }
    say("== roundtrip (real data): textures %zu pass %zu fail %zu skipped(no data/plan) %zu\n", n, pass, fail, skipped);
    say("stored bytes checked %llu, valid-region bytes %llu, padding bytes %llu, nonzero padding bytes %llu (in %zu textures)\n",
        (unsigned long long)bytes, (unsigned long long)valid, (unsigned long long)pad, (unsigned long long)pad_nz, pad_nonzero_tex);
    return fail ? 1 : 0;
}

/* ---- dump ---------------------------------------------------------------------------------------------- */

static int decode_level_rgba(const row_t* r, const sr_tex_plan* plan, uint32_t m, const uint8_t* S, size_t sz, uint8_t** out) {
    const sr_tex_level_plan* l = &plan->level[m];
    const sr_tex_format_info* fi = sr_tex_format_info_get(plan->desc.format.fmt);
    uint8_t* L = (uint8_t*)malloc((size_t)l->linear_size + 1);
    uint8_t* rgba = (uint8_t*)malloc((size_t)l->width * l->height * 4 + 4);
    if (sr_tex_deswizzle_level(plan, 0, m, S, sz, L, 0, 0)) { free(L); free(rgba); return -1; }
    int st = 0; uint32_t eb = 0;
    switch (fi->codec) {
    case SR_TEX_CODEC_ASTC: st = sr_tex_decode_astc_ldr(L, fi->block_w, fi->block_h, l->width, l->height, plan->desc.format.srgb, rgba, &eb); break;
    case SR_TEX_CODEC_BC1: st = sr_tex_decode_bc1(L, l->width, l->height, rgba); break;
    case SR_TEX_CODEC_BC3: st = sr_tex_decode_bc3(L, l->width, l->height, rgba); break;
    case SR_TEX_CODEC_BC7: st = sr_tex_decode_bc7(L, l->width, l->height, rgba); break;
    case SR_TEX_CODEC_RAW: st = sr_tex_raw_to_rgba8(plan->desc.format.fmt, L, l->width, l->height, rgba); break;
    default: st = -1;
    }
    free(L);
    if (st < 0 && fi->codec != SR_TEX_CODEC_BC7) { free(rgba); return st; }
    *out = rgba;
    return (int)eb;
}

static void mkdirs(const char* p) { char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", p); for (char* c = tmp + 3; *c; c++) if (*c == '\\' || *c == '/') { *c = 0; _mkdir(tmp); *c = '\\'; } _mkdir(tmp); }

static int cmd_dump(void) {
    char dir[1024]; snprintf(dir, sizeof dir, "%s\\dump", g_out); mkdirs(dir);
    char* ids = g_ids ? _strdup(g_ids) : NULL; if (!ids) { fprintf(stderr, "dump needs --ids\n"); return 2; }
    for (char* tok = strtok(ids, ","); tok; tok = strtok(NULL, ",")) {
        int id = atoi(tok);
        for (size_t i = 0; i < g_nrows; i++) {
            row_t* r = &g_rows[i]; if (r->id != id) continue;
            sr_tex_plan plan;
            int st = sr_tex_plan_from_unity_blob_ex(r->fmt, r->cs, r->w, r->h, r->mips, r->image_count > 0 ? (uint32_t)r->image_count : 1, r->blob, r->blob_len, r->complete, &plan);
            size_t sz; uint8_t* S = read_tex(r, &sz);
            if (!S || st) { say("dump id=%d: no data or plan status %d\n", id, st); free(S); continue; }
            for (uint32_t m = 0; m < plan.desc.levels; m++) {
                uint8_t* rgba; int eb = decode_level_rgba(r, &plan, m, S, sz, &rgba);
                if (eb < 0) { say("dump id=%d mip %u: decode status %d\n", id, m, eb); continue; }
                char path[1200]; snprintf(path, sizeof path, "%s\\%05d_m%u.png", dir, id, m);
                png_write_rgba(path, rgba, plan.level[m].width, plan.level[m].height);
                say("dump id=%d %s %ux%u fmt=%d mip %u -> %s (error blocks %d)\n", id, r->name, plan.level[m].width, plan.level[m].height, r->fmt, m, path, eb);
                free(rgba);
            }
            free(S);
        }
    }
    return 0;
}

/* ---- astc oracle --------------------------------------------------------------------------------------- */

typedef struct { int tex_id; uint32_t mip; uint64_t first, count; } seg_t;
typedef struct {
    uint32_t bw, bh; int srgb; size_t cap, n; uint8_t* blocks; seg_t* segs; size_t nseg, segcap;
} batch_t;

typedef struct { const uint8_t* blocks; uint8_t* out; uint32_t bw, bh; int srgb; uint64_t b0, b1, total; uint32_t errs; } job_t;
#ifdef _WIN32
static DWORD WINAPI worker(LPVOID p) {
    job_t* j = (job_t*)p; uint32_t e = 0;
    uint64_t n = j->b1 - j->b0;
    sr_tex_decode_astc_ldr(j->blocks + 16 * j->b0, j->bw, j->bh, (uint32_t)(n * j->bw), j->bh, j->srgb, j->out + j->b0 * j->bw * j->bh * 4, &e);
    j->errs = e; return 0;
}
#endif

/* NOTE: the decode of a row of N blocks laid out as one image row writes pixel (x,y) at y*width+x, so the
 * per-thread slice (a row of n blocks) is NOT contiguous in the full image. Each job therefore decodes into its
 * own buffer and the caller scatters it. */
static void decode_parallel(const uint8_t* blocks, uint8_t* out, uint32_t bw, uint32_t bh, int srgb, uint64_t N, int threads, uint32_t* errs) {
    if (threads < 1) threads = 1;
    job_t jobs[32]; HANDLE th[32]; uint8_t* part[32]; if (threads > 32) threads = 32;
    uint64_t per = (N + threads - 1) / threads;
    for (int t = 0; t < threads; t++) {
        uint64_t b0 = (uint64_t)t * per, b1 = b0 + per > N ? N : b0 + per; if (b0 > N) b0 = N; if (b1 < b0) b1 = b0;
        uint64_t n = b1 - b0;
        part[t] = (uint8_t*)malloc((size_t)(n * bw * bh * 4) + 4);
        jobs[t].blocks = blocks; jobs[t].out = part[t] - b0 * bw * bh * 4; jobs[t].bw = bw; jobs[t].bh = bh; jobs[t].srgb = srgb; jobs[t].b0 = b0; jobs[t].b1 = b1; jobs[t].errs = 0;
        th[t] = n ? CreateThread(NULL, 0, worker, &jobs[t], 0, NULL) : NULL;
    }
    uint32_t e = 0;
    for (int t = 0; t < threads; t++) {
        if (th[t]) { WaitForSingleObject(th[t], INFINITE); CloseHandle(th[t]); }
        e += jobs[t].errs;
        uint64_t n = jobs[t].b1 - jobs[t].b0;
        for (uint32_t y = 0; y < bh; y++)
            memcpy(out + ((uint64_t)y * N * bw + jobs[t].b0 * bw) * 4, part[t] + (uint64_t)y * n * bw * 4, (size_t)(n * bw * 4));
        free(part[t]);
    }
    *errs = e;
}

static uint64_t g_blocks_total, g_blocks_bad, g_err_blocks_ours, g_batches;
static char g_bad_log[64][256]; static int g_nbad_log;

static int run_oracle(const char* in, const char* outp, int srgb) {
    char cmd[2048]; snprintf(cmd, sizeof cmd, "\"%s\" %s \"%s\" \"%s\" -j %d -silent", g_oracle, srgb ? "-ds" : "-dl", in, outp, g_threads);
    STARTUPINFOA si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

static int flush_batch(batch_t* b) {
    if (!b->n) return 0;
    g_batches++;
    char in[1024], outp[1024]; snprintf(in, sizeof in, "%s\\work\\batch.astc", g_out); snprintf(outp, sizeof outp, "%s\\work\\batch.ktx", g_out);
    uint64_t W = b->n * b->bw;
    FILE* f = fopen(in, "wb"); if (!f) return -1;
    uint8_t hd[16] = {0x13, 0xAB, 0xA1, 0x5C, (uint8_t)b->bw, (uint8_t)b->bh, 1, (uint8_t)W, (uint8_t)(W >> 8), (uint8_t)(W >> 16), (uint8_t)b->bh, 0, 0, 1, 0, 0};
    fwrite(hd, 1, 16, f); fwrite(b->blocks, 16, b->n, f); fclose(f);
    int rc = run_oracle(in, outp, b->srgb);
    if (rc) { say("oracle failed rc=%d\n", rc); return -2; }
    f = fopen(outp, "rb"); if (!f) return -3;
    fseek(f, 0, SEEK_END); long fs = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* kt = (uint8_t*)malloc((size_t)fs);
    if (fread(kt, 1, (size_t)fs, f) != (size_t)fs) return -4;
    fclose(f);
    uint32_t kv = *(uint32_t*)(kt + 60);
    const uint8_t* ref = kt + 64 + kv + 4; uint8_t* xref = NULL;
    uint32_t kw = *(uint32_t*)(kt + 36), kh = *(uint32_t*)(kt + 40);
    if (kw != W || kh != b->bh) { say("oracle size mismatch %ux%u vs %llux%u\n", kw, kh, (unsigned long long)W, b->bh); return -5; }
    /* astcenc writes a 3-component KTX when every alpha is 255; expand to RGBA so the compare is uniform. */
    uint32_t gl_fmt = *(uint32_t*)(kt + 24);
    if (gl_fmt == 0x1907) {
        uint8_t* x = (uint8_t*)malloc((size_t)(W * b->bh * 4));
        for (uint64_t i = 0; i < W * b->bh; i++) { x[4*i] = ref[3*i]; x[4*i+1] = ref[3*i+1]; x[4*i+2] = ref[3*i+2]; x[4*i+3] = 255; }
        ref = x; xref = x;
    } else if (gl_fmt == 0x1903) { /* GL_RED: grayscale, opaque */
        uint8_t* x = (uint8_t*)malloc((size_t)(W * b->bh * 4));
        for (uint64_t i = 0; i < W * b->bh; i++) { x[4*i] = x[4*i+1] = x[4*i+2] = ref[i]; x[4*i+3] = 255; }
        ref = x; xref = x;
    } else if (gl_fmt == 0x8227) { /* GL_RG: grayscale plus alpha */
        uint8_t* x = (uint8_t*)malloc((size_t)(W * b->bh * 4));
        for (uint64_t i = 0; i < W * b->bh; i++) { x[4*i] = x[4*i+1] = x[4*i+2] = ref[2*i]; x[4*i+3] = ref[2*i+1]; }
        ref = x; xref = x;
    } else if (gl_fmt != 0x1908) { say("oracle KTX glFormat 0x%x unsupported\n", gl_fmt); return -6; }
    uint8_t* ours = (uint8_t*)malloc((size_t)(W * b->bh * 4));
    uint32_t errs = 0;
    decode_parallel(b->blocks, ours, b->bw, b->bh, b->srgb, b->n, g_threads, &errs);
    g_err_blocks_ours += errs;
    /* compare per block */
    for (size_t s = 0; s < b->nseg; s++) {
        seg_t* sg = &b->segs[s]; int seg_bad = 0;
        for (uint64_t k = 0; k < sg->count; k++) {
            uint64_t bi = sg->first + k; int diff = 0, magenta_ref = 1;
            for (uint32_t y = 0; y < b->bh && !diff; y++) {
                const uint8_t* a = ours + ((uint64_t)y * W + bi * b->bw) * 4; const uint8_t* c = ref + ((uint64_t)y * W + bi * b->bw) * 4;
                if (memcmp(a, c, (size_t)b->bw * 4)) diff = 1;
            }
            (void)magenta_ref;
            g_blocks_total++;
            if (diff) {
                g_blocks_bad++; seg_bad++;
                if (g_nbad_log < 64) {
                    const uint8_t* bl = b->blocks + 16 * bi; char hx[40]; for (int q = 0; q < 16; q++) sprintf(hx + 2 * q, "%02x", bl[q]);
                    const uint8_t* a = ours + (bi * b->bw) * 4; const uint8_t* c = ref + (bi * b->bw) * 4;
                    snprintf(g_bad_log[g_nbad_log++], 256, "tex %d mip %u block %llu bytes %s ours %d,%d,%d,%d oracle %d,%d,%d,%d", sg->tex_id, sg->mip, (unsigned long long)k, hx,
                             a[0], a[1], a[2], a[3], c[0], c[1], c[2], c[3]);
                }
            }
        }
        (void)seg_bad;
    }
    free(ours); free(kt); free(xref);
    b->n = 0; b->nseg = 0;
    return 0;
}

static int cmd_astc(void) {
    char wd[1024]; snprintf(wd, sizeof wd, "%s\\work", g_out); mkdirs(wd);
    batch_t bt[6]; memset(bt, 0, sizeof bt);
    const size_t BCAP = 1 << 18;
    for (int k = 0; k < 6; k++) { bt[k].cap = BCAP; bt[k].blocks = (uint8_t*)malloc(BCAP * 16); bt[k].segcap = 4096; bt[k].segs = (seg_t*)malloc(sizeof(seg_t) * 4096); }
    size_t textures = 0, skipped = 0;
    int status = 0;
    for (size_t i = 0; i < g_nrows && !status; i++) {
        row_t* r = &g_rows[i]; if (!selected(r, i)) continue;
        sr_tex_format f; if (sr_tex_format_from_unity(r->fmt, r->cs, &f)) continue;
        const sr_tex_format_info* fi = sr_tex_format_info_get(f.fmt);
        if (fi->codec != SR_TEX_CODEC_ASTC) continue;
        sr_tex_plan plan;
        int st = sr_tex_plan_from_unity_blob_ex(r->fmt, r->cs, r->w, r->h, r->mips, r->image_count > 0 ? (uint32_t)r->image_count : 1, r->blob, r->blob_len, r->complete, &plan);
        size_t sz = 0; uint8_t* S = (r->stsize || r->inline_file[0]) ? read_tex(r, &sz) : NULL;
        if (!S || st) { skipped++; free(S); continue; }
        textures++;
        int key = (fi->block_w == 4 ? 0 : fi->block_w == 5 ? 1 : 2) * 2 + (f.srgb ? 1 : 0);
        batch_t* b = &bt[key]; b->bw = fi->block_w; b->bh = fi->block_h; b->srgb = f.srgb;
        for (uint32_t lm = 0; lm < plan.desc.levels * plan.desc.layers; lm++) {
            const uint32_t m = lm % plan.desc.levels, lay = lm / plan.desc.levels;
            const sr_tex_level_plan* l = &plan.level[m];
            uint64_t nb = (uint64_t)l->blocks_w * l->blocks_h;
            uint8_t* L = (uint8_t*)malloc((size_t)l->linear_size);
            if (sr_tex_deswizzle_level(&plan, lay, m, S, sz, L, 0, 0)) { say("deswizzle failed id=%d\n", r->id); status = 1; free(L); break; }
            uint64_t done = 0;
            while (done < nb) {
                if (b->n == b->cap) { if (flush_batch(b)) { status = 1; break; } }
                uint64_t take = nb - done < b->cap - b->n ? nb - done : b->cap - b->n;
                memcpy(b->blocks + 16 * b->n, L + 16 * done, (size_t)(16 * take));
                if (b->nseg == b->segcap) { b->segcap *= 2; b->segs = (seg_t*)realloc(b->segs, sizeof(seg_t) * b->segcap); }
                b->segs[b->nseg++] = (seg_t){r->id, m, b->n, take};
                b->n += take; done += take;
            }
            free(L);
        }
        free(S);
        if (g_limit && (long)textures >= g_limit) break;
    }
    for (int k = 0; k < 6 && !status; k++) if (flush_batch(&bt[k])) status = 1;
    say("== astc oracle (astcenc 5.7.1 %s): textures %zu skipped %zu, blocks compared %llu, mismatching blocks %llu, batches %llu, error-color blocks in ours %llu\n",
        g_oracle, textures, skipped, (unsigned long long)g_blocks_total, (unsigned long long)g_blocks_bad, (unsigned long long)g_batches, (unsigned long long)g_err_blocks_ours);
    for (int k = 0; k < g_nbad_log; k++) say("  %s\n", g_bad_log[k]);
    return status ? 2 : (g_blocks_bad ? 1 : 0);
}

/* ---- bc7 quality ---------------------------------------------------------------------------------------- */

static int cmd_bc7(void) {
    size_t n = 0; double sum_psnr = 0, min_psnr = 1e9; double t_enc = 0; uint64_t pix = 0;
    for (size_t i = 0; i < g_nrows; i++) {
        row_t* r = &g_rows[i]; if (!selected(r, i)) continue;
        sr_tex_format f; if (sr_tex_format_from_unity(r->fmt, r->cs, &f)) continue;
        if (sr_tex_format_info_get(f.fmt)->codec != SR_TEX_CODEC_ASTC) continue;
        sr_tex_plan plan; if (sr_tex_plan_from_unity_blob_ex(r->fmt, r->cs, r->w, r->h, r->mips, r->image_count > 0 ? (uint32_t)r->image_count : 1, r->blob, r->blob_len, r->complete, &plan)) continue;
        size_t sz; uint8_t* S = read_tex(r, &sz); if (!S) continue;
        uint8_t* rgba; if (decode_level_rgba(r, &plan, 0, S, sz, &rgba) < 0) { free(S); continue; }
        uint32_t w = plan.level[0].width, h = plan.level[0].height;
        uint8_t* bc = (uint8_t*)malloc(((w + 3) / 4) * ((h + 3) / 4) * 16); uint8_t* dec = (uint8_t*)malloc((size_t)w * h * 4);
        LARGE_INTEGER a, b2, fr; QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&a);
        sr_tex_encode_bc7(rgba, w, h, 1, bc);
        QueryPerformanceCounter(&b2); t_enc += (double)(b2.QuadPart - a.QuadPart) / (double)fr.QuadPart; pix += (uint64_t)w * h;
        sr_tex_decode_bc7(bc, w, h, dec);
        double se = 0; for (size_t k = 0; k < (size_t)w * h * 4; k++) { double d = (double)rgba[k] - dec[k]; se += d * d; }
        double mse = se / ((double)w * h * 4); double psnr = mse < 1e-9 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
        sum_psnr += psnr; if (psnr < min_psnr) min_psnr = psnr; n++;
        free(S); free(rgba); free(bc); free(dec);
        if (g_limit && (long)n >= g_limit) break;
    }
    say("== bc7 transcode quality (ASTC mip0 -> BC7 mode 6, quality 1): textures %zu, mean PSNR %.2f dB, min %.2f dB, encode speed %.1f Mpix/s\n", n, n ? sum_psnr / n : 0.0, min_psnr, t_enc > 0 ? pix / t_enc / 1e6 : 0.0);
    return 0;
}

static void usage(void) { fprintf(stderr, "usage: texture_check survey|roundtrip|astc|bc7|dump [opts]\n"); }

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const char* cmd = argv[1];
    for (int i = 2; i < argc; i++) {
        #define OPT(n) (strcmp(argv[i], n) == 0 && i + 1 < argc)
        if (OPT("--manifest")) g_manifest = argv[++i]; else if (OPT("--aa")) g_aa = argv[++i]; else if (OPT("--out")) g_out = argv[++i];
        else if (OPT("--report")) g_report = argv[++i]; else if (OPT("--limit")) g_limit = atol(argv[++i]); else if (OPT("--every")) g_every = atol(argv[++i]);
        else if (OPT("--first")) g_first = atol(argv[++i]); else if (OPT("--last")) g_last = atol(argv[++i]); else if (OPT("--fmt")) g_fmt = atol(argv[++i]);
        else if (OPT("--shard")) { sscanf(argv[++i], "%d/%d", &g_shard_i, &g_shard_n); } else if (OPT("--threads")) g_threads = atoi(argv[++i]);
        else if (OPT("--oracle")) g_oracle = argv[++i]; else if (OPT("--ids")) g_ids = argv[++i];
        else { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
    }
    below_normal();
    if (g_report) g_rep = fopen(g_report, "w");
    if (load_manifest()) return 2;
    int rc;
    if (!strcmp(cmd, "survey")) rc = cmd_survey();
    else if (!strcmp(cmd, "roundtrip")) rc = cmd_roundtrip();
    else if (!strcmp(cmd, "astc")) rc = cmd_astc();
    else if (!strcmp(cmd, "bc7")) rc = cmd_bc7();
    else if (!strcmp(cmd, "dump")) rc = cmd_dump();
    else { usage(); rc = 2; }
    if (g_rep) fclose(g_rep);
    return rc;
}
