/* Texture path unit tests (task-11). Our own code. No game data: every input is generated here.
 *
 * Covers: GOB bit interleave against explicit arithmetic; swizzle against an independent address model for many
 * formats, sizes and full mip chains; swizzle/deswizzle round trips (2D, arrays, cube, 3D, custom pitches);
 * plan invariants for every format; Unity blob parsing (block-linear, empty-blob linear, RGB24); ASTC void-extent,
 * edge cropping and error handling; BC1/BC3 hand-built blocks; BC7 transcode quality; sr_tex_prepare_upload
 * for NATIVE, RGBA8, BC7 and AUTO targets. Prints one "PASS/FAIL name" line per test and a summary line.
 * Usage: texture_unit [--verbose]
 */
#include "texture.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass, g_verbose;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 0; } } while (0)
#define RUN(fn) do { int ok_ = fn(); if (ok_) { g_pass++; printf("PASS %s\n", #fn); } else { printf("FAIL %s\n", #fn); } } while (0)

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 16); }
static void fill_nonzero(uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(1 + rnd() % 255); }

/* ---- independent address model (one layer, one mip) ------------------------------------------------------ */

static uint32_t ref_gob_inner(uint32_t x, uint32_t y) {   /* x 0..63, y 0..7 */
    return (x / 32) * 256 + ((y / 2) % 4) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}
static uint64_t ref_addr(uint64_t xbyte, uint64_t y, uint64_t z, uint64_t rowbytes, uint64_t hb, uint32_t bh, uint32_t bd) {
    uint64_t gobs_w = (rowbytes + 63) / 64;
    uint64_t blocks_y = (hb + 8ull * bh - 1) / (8ull * bh);
    uint64_t gy = y / 8, by = gy / bh, gin = gy % bh, bz = z / bd, zin = z % bd;
    uint64_t block_index = (bz * blocks_y + by) * gobs_w + xbyte / 64;
    return block_index * 512ull * bh * bd + (zin * bh + gin) * 512ull + ref_gob_inner((uint32_t)(xbyte % 64), (uint32_t)(y % 8));
}

static int t_gob_offset(void) {
    uint8_t seen[512]; memset(seen, 0, sizeof seen);
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint32_t o = sr_tex_gob_offset(x, y);
            CHECK(o < 512, "offset %u out of GOB", o);
            CHECK(o == ref_gob_inner(x, y), "x=%u y=%u got %u want %u", x, y, o, ref_gob_inner(x, y));
            CHECK(!seen[o], "offset %u hit twice", o);
            seen[o] = 1;
        }
    return 1;
}

static int t_default_bh(void) {
    CHECK(sr_tex_default_bh_log2(1) == 0, "h=1");
    CHECK(sr_tex_default_bh_log2(10) == 0, "h=10");
    CHECK(sr_tex_default_bh_log2(11) == 1, "h=11");
    CHECK(sr_tex_default_bh_log2(21) == 1, "h=21");
    CHECK(sr_tex_default_bh_log2(22) == 2, "h=22");
    CHECK(sr_tex_default_bh_log2(43) == 3, "h=43");
    CHECK(sr_tex_default_bh_log2(86) == 4, "h=86");
    CHECK(sr_tex_default_bh_log2(4096) == 4, "h=4096");
    return 1;
}

/* Swizzle random linear data and compare every byte against the independent model. */
static int swizzle_vs_model(sr_tex_fmt fmt, uint32_t w, uint32_t h, uint32_t levels, int32_t bh_log2) {
    sr_tex_surface_desc d; memset(&d, 0, sizeof d);
    d.format.fmt = fmt; d.width = w; d.height = h; d.depth = 1; d.layers = 1; d.levels = levels; d.dim = SR_TEX_DIM_2D;
    d.block_height_log2 = bh_log2; d.block_depth_log2 = -1;
    sr_tex_plan p;
    if (sr_tex_plan_build(&d, &p)) { printf("  plan_build failed fmt=%d %ux%u\n", fmt, w, h); return 0; }
    const sr_tex_format_info* fi = sr_tex_format_info_get(fmt);
    uint8_t* swz = (uint8_t*)calloc(1, (size_t)p.total_size);
    size_t nonzero_expected = 0;
    uint64_t end_prev = 0;
    for (uint32_t m = 0; m < levels; m++) {
        const sr_tex_level_plan* l = &p.level[m];
        /* independent per-mip block height and size */
        uint32_t bh = 1u << (bh_log2 >= 0 ? (uint32_t)bh_log2 : sr_tex_default_bh_log2((h + fi->block_h - 1) / fi->block_h));
        while (bh > 1 && 8u * (bh / 2) >= l->blocks_h) bh /= 2;
        if (l->offset != end_prev) { printf("  mip %u offset %llu want %llu (fmt %d %ux%u)\n", m, (unsigned long long)l->offset, (unsigned long long)end_prev, fmt, w, h); free(swz); return 0; }
        uint64_t rowb = (uint64_t)l->blocks_w * fi->bytes_per_block;
        uint64_t want_size = ((rowb + 63) / 64) * ((l->blocks_h + 8ull * bh - 1) / (8ull * bh)) * 512ull * bh;
        if (l->size != want_size) { printf("  mip %u size %llu want %llu\n", m, (unsigned long long)l->size, (unsigned long long)want_size); free(swz); return 0; }
        end_prev += l->size;
        uint8_t* lin = (uint8_t*)malloc((size_t)l->linear_size);
        fill_nonzero(lin, (size_t)l->linear_size);
        if (sr_tex_swizzle_level(&p, 0, m, lin, 0, 0, swz, (size_t)p.total_size)) { printf("  swizzle failed\n"); free(lin); free(swz); return 0; }
        nonzero_expected += (size_t)l->linear_size;
        for (uint64_t y = 0; y < l->blocks_h; y++)
            for (uint64_t x = 0; x < rowb; x++) {
                uint64_t a = l->offset + ref_addr(x, y, 0, rowb, l->blocks_h, bh, 1);
                if (swz[a] != lin[y * rowb + x]) {
                    printf("  mismatch fmt %d %ux%u mip %u (x=%llu,y=%llu) addr %llu\n", fmt, w, h, m, (unsigned long long)x, (unsigned long long)y, (unsigned long long)a);
                    free(lin); free(swz); return 0;
                }
            }
        free(lin);
    }
    size_t nz = 0;
    for (uint64_t i = 0; i < p.total_size; i++) nz += swz[i] != 0;
    free(swz);
    if (nz != nonzero_expected) { printf("  stray bytes: %zu nonzero, want %zu (fmt %d %ux%u)\n", nz, nonzero_expected, fmt, w, h); return 0; }
    return 1;
}

static int t_swizzle_model(void) {
    static const sr_tex_fmt fmts[] = { SR_TEX_FMT_R8, SR_TEX_FMT_RG8, SR_TEX_FMT_RGB8, SR_TEX_FMT_RGBA8, SR_TEX_FMT_RGBA16_FLOAT, SR_TEX_FMT_RGBA32_FLOAT,
        SR_TEX_FMT_BC1_RGB, SR_TEX_FMT_BC3, SR_TEX_FMT_BC7, SR_TEX_FMT_ASTC_4x4, SR_TEX_FMT_ASTC_6x6, SR_TEX_FMT_ASTC_8x5, SR_TEX_FMT_ASTC_12x12,
        SR_TEX_FMT_ASTC_10x6, SR_TEX_FMT_DEPTH24_STENCIL8 };
    static const uint32_t dims[][2] = { {1,1}, {7,5}, {64,64}, {100,37}, {256,300}, {33,1000}, {1024,16}, {513,129}, {2048,2048} };
    int combos = 0;
    for (size_t f = 0; f < sizeof fmts / sizeof *fmts; f++)
        for (size_t i = 0; i < sizeof dims / sizeof *dims; i++) {
            uint32_t w = dims[i][0], h = dims[i][1];
            if (w * h > 1100000 && fmts[f] > SR_TEX_FMT_RGBA8) continue;      /* keep it quick: big sizes only for small formats */
            uint32_t levels = 1; while (levels < SR_TEX_MAX_LEVELS && ((w >> levels) | (h >> levels))) levels++;
            for (int bhl = -1; bhl <= 4; bhl += (bhl < 0 ? 1 : 2)) {
                int ok = swizzle_vs_model(fmts[f], w, h, levels, bhl);
                CHECK(ok, "model mismatch fmt=%d %ux%u bh=%d", fmts[f], w, h, bhl);
                combos++;
            }
        }
    if (g_verbose) printf("  %d format/size/bh combinations\n", combos);
    return 1;
}

/* ---- round trips: arrays, cube, 3D, pitches ------------------------------------------------------------------ */

static int roundtrip(sr_tex_fmt fmt, uint32_t w, uint32_t h, uint32_t depth, uint32_t layers, uint32_t levels, sr_tex_dim dim,
                     int32_t bhl, int32_t bdl, int pitched) {
    sr_tex_surface_desc d; memset(&d, 0, sizeof d);
    d.format.fmt = fmt; d.width = w; d.height = h; d.depth = depth; d.layers = layers; d.levels = levels; d.dim = dim;
    d.block_height_log2 = bhl; d.block_depth_log2 = bdl;
    sr_tex_plan p;
    if (sr_tex_plan_build(&d, &p)) { printf("  plan_build failed\n"); return 0; }
    const sr_tex_format_info* fi = sr_tex_format_info_get(fmt);
    uint8_t* swz = (uint8_t*)calloc(1, (size_t)p.total_size);
    int ok = 1;
    /* layers must not overlap: layer_stride covers one layer's mips */
    if (p.desc.layers > 1) {
        uint64_t last = p.level[levels - 1].offset + p.level[levels - 1].size;
        if (p.layer_stride < last) { printf("  layer stride %llu < mip chain %llu\n", (unsigned long long)p.layer_stride, (unsigned long long)last); ok = 0; }
    }
    typedef struct { uint8_t* data; uint64_t rowp, slicep; } keep_t;
    keep_t* keep = (keep_t*)calloc(p.desc.layers * levels, sizeof(keep_t));
    for (uint32_t lay = 0; lay < p.desc.layers && ok; lay++)
        for (uint32_t m = 0; m < levels && ok; m++) {
            const sr_tex_level_plan* l = &p.level[m];
            uint64_t rowb = (uint64_t)l->blocks_w * fi->bytes_per_block;
            uint64_t rowp = pitched ? rowb + 24 : 0, slicep = pitched ? (rowb + 24) * l->blocks_h + 40 : 0;
            uint64_t rp = rowp ? rowp : rowb, sp = slicep ? slicep : rp * l->blocks_h;
            size_t sz = (size_t)(sp * l->depth);
            uint8_t* lin = (uint8_t*)malloc(sz);
            fill_nonzero(lin, sz);
            keep_t* k = &keep[lay * levels + m]; k->data = lin; k->rowp = rowp; k->slicep = slicep;
            if (sr_tex_swizzle_level(&p, lay, m, lin, rowp, slicep, swz, (size_t)p.total_size)) { printf("  swizzle failed lay %u mip %u\n", lay, m); ok = 0; }
        }
    for (uint32_t lay = 0; lay < p.desc.layers && ok; lay++)
        for (uint32_t m = 0; m < levels && ok; m++) {
            const sr_tex_level_plan* l = &p.level[m];
            keep_t* k = &keep[lay * levels + m];
            uint64_t rowb = (uint64_t)l->blocks_w * fi->bytes_per_block;
            uint64_t rp = k->rowp ? k->rowp : rowb, sp = k->slicep ? k->slicep : rp * l->blocks_h;
            uint8_t* back = (uint8_t*)calloc(1, (size_t)(sp * l->depth));
            if (sr_tex_deswizzle_level(&p, lay, m, swz, (size_t)p.total_size, back, k->rowp, k->slicep)) { printf("  deswizzle failed\n"); ok = 0; }
            for (uint32_t z = 0; z < l->depth && ok; z++)
                for (uint64_t y = 0; y < l->blocks_h && ok; y++)
                    if (memcmp(back + z * sp + y * rp, k->data + z * sp + y * rp, (size_t)rowb)) {
                        printf("  round trip mismatch fmt %d %ux%ux%u lay %u mip %u z %u y %llu\n", fmt, w, h, depth, lay, m, z, (unsigned long long)y); ok = 0;
                    }
            free(back);
        }
    for (uint32_t i = 0; i < p.desc.layers * levels; i++) free(keep[i].data);
    free(keep); free(swz);
    return ok;
}

static int t_roundtrip_formats(void) {
    /* every format in the table, odd sizes, full chain */
    int n = 0;
    for (int f = SR_TEX_FMT_NONE + 1; f < SR_TEX_FMT__COUNT; f++) {
        CHECK(roundtrip((sr_tex_fmt)f, 75, 53, 1, 1, 7, SR_TEX_DIM_2D, -1, -1, 0), "fmt %d", f);
        CHECK(roundtrip((sr_tex_fmt)f, 130, 260, 1, 1, 9, SR_TEX_DIM_2D, 3, -1, 1), "fmt %d pitched", f);
        n++;
    }
    if (g_verbose) printf("  %d formats\n", n);
    return 1;
}
static int t_roundtrip_block_heights(void) {
    for (int bh = 0; bh <= 5; bh++) {
        CHECK(roundtrip(SR_TEX_FMT_RGBA8, 300, 700, 1, 1, 10, SR_TEX_DIM_2D, bh, -1, 0), "bh %d RGBA8", bh);
        CHECK(roundtrip(SR_TEX_FMT_ASTC_6x6, 512, 512, 1, 1, 10, SR_TEX_DIM_2D, bh, -1, 0), "bh %d ASTC6x6", bh);
        CHECK(roundtrip(SR_TEX_FMT_BC1_RGB, 1024, 64, 1, 1, 11, SR_TEX_DIM_2D, bh, -1, 0), "bh %d BC1", bh);
    }
    return 1;
}
static int t_roundtrip_arrays(void) {
    CHECK(roundtrip(SR_TEX_FMT_RGBA8, 100, 100, 1, 4, 7, SR_TEX_DIM_2D, -1, -1, 0), "array");
    CHECK(roundtrip(SR_TEX_FMT_ASTC_8x8, 256, 256, 1, 6, 5, SR_TEX_DIM_CUBE, -1, -1, 1), "cube astc");
    CHECK(roundtrip(SR_TEX_FMT_BC7, 128, 128, 1, 6, 8, SR_TEX_DIM_CUBE, 2, -1, 0), "cube bc7");
    CHECK(roundtrip(SR_TEX_FMT_R8, 33, 17, 1, 3, 6, SR_TEX_DIM_2D, 0, -1, 0), "array r8");
    return 1;
}
static int t_roundtrip_3d(void) {
    CHECK(roundtrip(SR_TEX_FMT_RGBA8, 32, 32, 5, 1, 6, SR_TEX_DIM_3D, -1, -1, 0), "3d auto");
    CHECK(roundtrip(SR_TEX_FMT_RG8, 50, 30, 9, 1, 6, SR_TEX_DIM_3D, 1, 2, 1), "3d explicit pitched");
    CHECK(roundtrip(SR_TEX_FMT_BC1_RGB, 64, 64, 16, 1, 7, SR_TEX_DIM_3D, 0, 3, 0), "3d bc1");
    CHECK(roundtrip(SR_TEX_FMT_RGBA16_FLOAT, 20, 20, 3, 2, 5, SR_TEX_DIM_3D, -1, -1, 0), "3d array");
    return 1;
}

static int t_plan_invariants(void) {
    for (int f = SR_TEX_FMT_NONE + 1; f < SR_TEX_FMT__COUNT; f++) {
        const sr_tex_format_info* fi = sr_tex_format_info_get((sr_tex_fmt)f);
        CHECK(fi && fi->name && fi->bytes_per_block > 0 && fi->block_w > 0 && fi->block_h > 0, "info fmt %d", f);
        if (fi->codec == SR_TEX_CODEC_ASTC) CHECK(fi->bytes_per_block == 16, "astc bpb fmt %d", f);
        static const uint32_t sz[][2] = { {1, 1}, {4, 4}, {513, 257}, {4096, 4096}, {1, 777} };
        for (size_t i = 0; i < 5; i++) {
            sr_tex_surface_desc d; memset(&d, 0, sizeof d);
            d.format.fmt = (sr_tex_fmt)f; d.width = sz[i][0]; d.height = sz[i][1]; d.layers = 1; d.depth = 1; d.levels = 1;
            while (d.levels < SR_TEX_MAX_LEVELS && ((d.width >> d.levels) | (d.height >> d.levels))) d.levels++;
            d.block_height_log2 = -1; d.block_depth_log2 = -1;
            sr_tex_plan p;
            CHECK(sr_tex_plan_build(&d, &p) == SR_TEX_OK, "plan fmt %d %ux%u", f, d.width, d.height);
            uint64_t end = 0, lend = 0;
            for (uint32_t m = 0; m < d.levels; m++) {
                CHECK(p.level[m].offset == end, "offset gap fmt %d mip %u", f, m);
                CHECK(p.level[m].linear_offset == lend, "linear offset gap fmt %d mip %u", f, m);
                CHECK(p.level[m].size >= p.level[m].linear_size, "swizzled smaller than linear fmt %d mip %u", f, m);
                CHECK(p.level[m].size % 512 == 0, "mip size not GOB multiple fmt %d", f);
                end += p.level[m].size; lend += p.level[m].linear_size;
            }
            CHECK(p.total_size == end && p.linear_total_size == lend, "totals fmt %d", f);
        }
    }
    sr_tex_plan p; sr_tex_surface_desc bad; memset(&bad, 0, sizeof bad);
    CHECK(sr_tex_plan_build(NULL, &p) == SR_TEX_E_ARG, "null desc");
    CHECK(sr_tex_plan_build(&bad, &p) < 0, "zero desc");
    bad.format.fmt = SR_TEX_FMT_RGBA8; bad.width = 4; bad.height = 4; bad.levels = 17;
    CHECK(sr_tex_plan_build(&bad, &p) == SR_TEX_E_ARG, "17 levels");
    return 1;
}

static int t_range_and_args(void) {
    sr_tex_surface_desc d; memset(&d, 0, sizeof d);
    d.format.fmt = SR_TEX_FMT_RGBA8; d.width = 64; d.height = 64; d.layers = 1; d.depth = 1; d.levels = 3; d.block_height_log2 = -1; d.block_depth_log2 = -1;
    sr_tex_plan p; CHECK(sr_tex_plan_build(&d, &p) == 0, "plan");
    uint8_t* swz = (uint8_t*)calloc(1, (size_t)p.total_size); uint8_t* lin = (uint8_t*)calloc(1, 64 * 64 * 4);
    CHECK(sr_tex_deswizzle_level(&p, 0, 2, swz, (size_t)p.level[2].offset, lin, 0, 0) == SR_TEX_E_RANGE, "mip 2 out of range");
    CHECK(sr_tex_deswizzle_level(&p, 1, 0, swz, (size_t)p.total_size, lin, 0, 0) == SR_TEX_E_ARG, "layer 1 of 1");
    CHECK(sr_tex_deswizzle_level(&p, 0, 3, swz, (size_t)p.total_size, lin, 0, 0) == SR_TEX_E_ARG, "level 3 of 3");
    CHECK(sr_tex_deswizzle_level(&p, 0, 0, NULL, 0, lin, 0, 0) == SR_TEX_E_ARG, "null swz");
    CHECK(sr_tex_deswizzle_level(&p, 0, 0, swz, (size_t)p.total_size, lin, 4, 0) == SR_TEX_E_ARG, "row pitch below tight");
    free(swz); free(lin);
    return 1;
}

/* ---- Unity platform blob ------------------------------------------------------------------------------------- */

static void put32(uint8_t* b, size_t off, uint32_t v) { b[off] = (uint8_t)v; b[off+1] = (uint8_t)(v >> 8); b[off+2] = (uint8_t)(v >> 16); b[off+3] = (uint8_t)(v >> 24); }
static void make_blob(const sr_tex_plan* p, uint8_t blob[92]) {
    memset(blob, 0, 92);
    put32(blob, 8, p->level[0].bh_log2); put32(blob, 12, 0x10007);
    for (uint32_t m = 0; m < p->desc.levels; m++) put32(blob, 16 + 4 * m, (uint32_t)p->level[m].offset);
    put32(blob, 84, (uint32_t)p->total_size);
}
static int unity_case(int ufmt, int srgb, uint32_t w, uint32_t h, uint32_t mips, int32_t bhl, sr_tex_fmt expect) {
    sr_tex_format f;
    if (sr_tex_format_from_unity(ufmt, srgb, &f)) { printf("  from_unity %d failed\n", ufmt); return 0; }
    if (ufmt == 3) f.fmt = SR_TEX_FMT_RGBA8;
    if (f.fmt != expect) { printf("  unity %d -> %d want %d\n", ufmt, f.fmt, expect); return 0; }
    sr_tex_surface_desc d; memset(&d, 0, sizeof d);
    d.format = f; d.width = w; d.height = h; d.depth = 1; d.layers = 1; d.levels = mips; d.block_height_log2 = bhl; d.block_depth_log2 = -1;
    sr_tex_plan ref; if (sr_tex_plan_build(&d, &ref)) return 0;
    uint8_t blob[92]; make_blob(&ref, blob);
    sr_tex_plan p;
    int r = sr_tex_plan_from_unity_blob(ufmt, srgb, w, h, mips, blob, 92, ref.total_size, &p);
    if (r != SR_TEX_OK) { printf("  from_blob rc %d (unity %d %ux%u)\n", r, ufmt, w, h); return 0; }
    if (p.total_size != ref.total_size || p.stored_linear) return 0;
    /* tampered offset, tampered total, short blob */
    uint8_t bad[92]; memcpy(bad, blob, 92); if (mips > 1) put32(bad, 20, (uint32_t)ref.level[1].offset + 512);
    if (mips > 1 && sr_tex_plan_from_unity_blob(ufmt, srgb, w, h, mips, bad, 92, ref.total_size, &p) != SR_TEX_E_MISMATCH) { printf("  tampered offset not detected\n"); return 0; }
    memcpy(bad, blob, 92); put32(bad, 84, (uint32_t)ref.total_size + 512);
    if (sr_tex_plan_from_unity_blob(ufmt, srgb, w, h, mips, bad, 92, ref.total_size, &p) != SR_TEX_E_MISMATCH) { printf("  tampered total not detected\n"); return 0; }
    if (sr_tex_plan_from_unity_blob(ufmt, srgb, w, h, mips, blob, 91, ref.total_size, &p) != SR_TEX_E_BLOB) { printf("  short blob accepted\n"); return 0; }
    if (sr_tex_plan_from_unity_blob(ufmt, srgb, w, h, mips, blob, 92, ref.total_size + 8, &p) != SR_TEX_E_MISMATCH) { printf("  wrong complete size accepted\n"); return 0; }
    return 1;
}
static int t_unity_blob(void) {
    CHECK(unity_case(50, 1, 256, 256, 9, 4, SR_TEX_FMT_ASTC_6x6), "astc 6x6");
    CHECK(unity_case(48, 0, 100, 60, 7, 1, SR_TEX_FMT_ASTC_4x4), "astc 4x4");
    CHECK(unity_case(12, 0, 512, 128, 10, 3, SR_TEX_FMT_BC3), "dxt5");
    CHECK(unity_case(4, 1, 64, 64, 7, 3, SR_TEX_FMT_RGBA8), "rgba32");
    CHECK(unity_case(3, 0, 40, 40, 6, 2, SR_TEX_FMT_RGBA8), "rgb24 stored as 4 bytes");
    return 1;
}
static int t_unity_empty_blob(void) {
    sr_tex_plan p;
    uint64_t total = 0; uint32_t w = 37, h = 21, mips = 6;
    for (uint32_t m = 0; m < mips; m++) { uint32_t mw = w >> m ? w >> m : 1, mh = h >> m ? h >> m : 1; total += (uint64_t)mw * mh * 4; }
    CHECK(sr_tex_plan_from_unity_blob(4, 0, w, h, mips, NULL, 0, total, &p) == SR_TEX_OK, "empty blob rgba32");
    CHECK(p.stored_linear == 1, "stored_linear");
    CHECK(p.total_size == total && p.level[1].offset == (uint64_t)w * h * 4, "packed offsets");
    CHECK(sr_tex_plan_from_unity_blob(4, 0, w, h, mips, NULL, 0, total + 1, &p) == SR_TEX_E_MISMATCH, "wrong size");
    /* deswizzle is a plain row copy */
    uint8_t* st = (uint8_t*)malloc((size_t)total); fill_nonzero(st, (size_t)total);
    CHECK(sr_tex_plan_from_unity_blob(4, 0, w, h, mips, NULL, 0, total, &p) == SR_TEX_OK, "again");
    for (uint32_t m = 0; m < mips; m++) {
        uint8_t* lin = (uint8_t*)malloc((size_t)p.level[m].linear_size);
        CHECK(sr_tex_deswizzle_level(&p, 0, m, st, (size_t)total, lin, 0, 0) == 0, "deswizzle linear");
        CHECK(!memcmp(lin, st + p.level[m].offset, (size_t)p.level[m].linear_size), "copy mip %u", m);
        uint8_t* back = (uint8_t*)calloc(1, (size_t)total);
        CHECK(sr_tex_swizzle_level(&p, 0, m, lin, 0, 0, back, (size_t)total) == 0, "swizzle linear");
        CHECK(!memcmp(back + p.level[m].offset, st + p.level[m].offset, (size_t)p.level[m].linear_size), "swizzle copy mip %u", m);
        free(lin); free(back);
    }
    free(st);
    /* RGB24 with an empty blob keeps 3 bytes per texel */
    CHECK(sr_tex_plan_from_unity_blob(3, 0, 8, 8, 1, NULL, 0, 8 * 8 * 3, &p) == SR_TEX_OK, "rgb24 empty");
    CHECK(p.desc.format.fmt == SR_TEX_FMT_RGB8, "rgb24 stays rgb8 when linear");
    return 1;
}

static int t_unity_cubemap(void) {
    /* Cubemap in the dump: empty blob, 6 faces, each face a full linear mip chain; m_CompleteImageSize is one face. */
    sr_tex_plan p; uint32_t w = 32, h = 32, mips = 6; uint64_t face = 0;
    for (uint32_t m = 0; m < mips; m++) { uint32_t mw = (w >> m) ? (w >> m) : 1, mh = (h >> m) ? (h >> m) : 1; face += (uint64_t)((mw + 3) / 4) * ((mh + 3) / 4) * 8; }
    CHECK(sr_tex_plan_from_unity_blob_ex(10, 1, w, h, mips, 6, NULL, 0, face, &p) == SR_TEX_OK, "cube plan");
    CHECK(p.stored_linear == 1 && p.desc.layers == 6 && p.desc.dim == SR_TEX_DIM_CUBE, "cube shape");
    CHECK(p.layer_stride == face && p.total_size == 6 * face, "cube strides %llu %llu", (unsigned long long)p.layer_stride, (unsigned long long)p.total_size);
    CHECK(sr_tex_plan_from_unity_blob_ex(10, 1, w, h, mips, 6, NULL, 0, 6 * face, &p) == SR_TEX_E_MISMATCH, "complete size is per face");
    CHECK(sr_tex_plan_from_unity_blob_ex(10, 1, w, h, mips, 0, NULL, 0, face, &p) == SR_TEX_E_ARG, "zero images");
    uint8_t* st = (uint8_t*)malloc((size_t)(6 * face)); fill_nonzero(st, (size_t)(6 * face));
    CHECK(sr_tex_plan_from_unity_blob_ex(10, 1, w, h, mips, 6, NULL, 0, face, &p) == SR_TEX_OK, "again");
    for (uint32_t lay = 0; lay < 6; lay++)
        for (uint32_t m = 0; m < mips; m++) {
            uint8_t* lin = (uint8_t*)malloc((size_t)p.level[m].linear_size);
            CHECK(sr_tex_deswizzle_level(&p, lay, m, st, (size_t)(6 * face), lin, 0, 0) == 0, "deswizzle face %u", lay);
            CHECK(!memcmp(lin, st + lay * face + p.level[m].offset, (size_t)p.level[m].linear_size), "face %u mip %u bytes", lay, m);
            free(lin);
        }
    free(st);
    return 1;
}
static int t_format_tables(void) {
    sr_tex_format f;
    CHECK(sr_tex_format_from_unity(50, 0, &f) == 0 && f.fmt == SR_TEX_FMT_ASTC_6x6, "unity 50");
    CHECK(sr_tex_format_from_unity(50, 1, &f) == 0 && f.srgb == 1, "unity 50 srgb");
    CHECK(sr_tex_format_from_unity(53, 0, &f) == 0 && f.fmt == SR_TEX_FMT_ASTC_12x12, "unity 53");
    CHECK(sr_tex_format_from_unity(9999, 0, &f) < 0, "unity unknown");
    CHECK(sr_tex_format_from_nvn(0xFFFFFFFFu, &f) == SR_TEX_E_FORMAT, "nvn unknown");
    CHECK(sr_tex_format_info_get(SR_TEX_FMT_NONE) == NULL && sr_tex_format_info_get(SR_TEX_FMT__COUNT) == NULL, "info range");
    return 1;
}

/* ---- ASTC --------------------------------------------------------------------------------------------------- */

static void void_extent(uint8_t b[16], uint16_t r, uint16_t g, uint16_t bl, uint16_t a) {
    b[0] = 0xFC; b[1] = 0xFD; memset(b + 2, 0xFF, 6);
    b[8] = (uint8_t)r; b[9] = (uint8_t)(r >> 8); b[10] = (uint8_t)g; b[11] = (uint8_t)(g >> 8);
    b[12] = (uint8_t)bl; b[13] = (uint8_t)(bl >> 8); b[14] = (uint8_t)a; b[15] = (uint8_t)(a >> 8);
}

static int t_astc_void_extent(void) {
    uint8_t blk[16], out[6 * 6 * 4]; uint32_t err = 99;
    void_extent(blk, 257 * 200, 257 * 100, 257 * 16, 257 * 255);
    CHECK(sr_tex_decode_astc_ldr(blk, 6, 6, 6, 6, 0, out, &err) == 0 && err == 0, "decode rc/err %u", err);
    for (int i = 0; i < 36; i++) CHECK(out[4*i] == 200 && out[4*i+1] == 100 && out[4*i+2] == 16 && out[4*i+3] == 255, "texel %d = %d,%d,%d,%d", i, out[4*i], out[4*i+1], out[4*i+2], out[4*i+3]);
    err = 99;
    CHECK(sr_tex_decode_astc_ldr(blk, 6, 6, 6, 6, 1, out, &err) == 0 && err == 0, "srgb decode");
    CHECK(out[0] == 200 && out[1] == 100 && out[2] == 16 && out[3] == 255, "srgb void extent %d,%d,%d,%d", out[0], out[1], out[2], out[3]);
    uint16_t px[6 * 6 * 4];
    CHECK(sr_tex_decode_astc_block(blk, 6, 6, 0, px) == 0, "block decode");
    CHECK(px[0] == 257 * 200 && px[3] == 257 * 255, "16-bit values %u %u", px[0], px[3]);
    return 1;
}

static int t_astc_edges(void) {
    /* 7x6 image with 4x4 blocks: 2x2 blocks of distinct colors, cropped to 7x6. */
    uint8_t blocks[4][16], out[7 * 6 * 4]; uint32_t err = 0;
    static const uint8_t cols[4][3] = { {10, 20, 30}, {40, 50, 60}, {70, 80, 90}, {100, 110, 120} };
    for (int i = 0; i < 4; i++) void_extent(blocks[i], 257 * cols[i][0], 257 * cols[i][1], 257 * cols[i][2], 257 * 255);
    CHECK(sr_tex_decode_astc_ldr(&blocks[0][0], 4, 4, 7, 6, 0, out, &err) == 0 && err == 0, "decode");
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 7; x++) {
            const uint8_t* want = cols[(y / 4) * 2 + x / 4]; const uint8_t* o = out + 4 * (y * 7 + x);
            CHECK(o[0] == want[0] && o[1] == want[1] && o[2] == want[2] && o[3] == 255, "pixel %d,%d = %d,%d,%d", x, y, o[0], o[1], o[2]);
        }
    return 1;
}

static int t_astc_errors(void) {
    uint8_t zero[16] = {0}, out[4 * 4 * 4]; uint32_t err = 0;
    sr_tex_decode_astc_ldr(zero, 4, 4, 4, 4, 0, out, &err);
    CHECK(err == 1, "all-zero block is reserved: error count %u", err);
    CHECK(out[0] == 255 && out[1] == 0 && out[2] == 255 && out[3] == 255, "error color %d,%d,%d,%d", out[0], out[1], out[2], out[3]);
    /* HDR void extent (D bit set) is not LDR: error color */
    uint8_t hdr[16]; void_extent(hdr, 0, 0, 0, 0); hdr[1] |= 0x02;
    err = 0; sr_tex_decode_astc_ldr(hdr, 4, 4, 4, 4, 0, out, &err);
    CHECK(err == 1 && out[0] == 255 && out[1] == 0, "hdr void extent: err %u", err);
    CHECK(sr_tex_decode_astc_ldr(NULL, 4, 4, 4, 4, 0, out, &err) < 0, "null blocks");
    CHECK(sr_tex_decode_astc_ldr(zero, 3, 3, 4, 4, 0, out, &err) < 0, "bad block size");
    return 1;
}

/* ---- BC ----------------------------------------------------------------------------------------------------- */

static int near1(int a, int b) { return abs(a - b) <= 1; }

static int t_bc1_bc3(void) {
    uint8_t blk[8] = { 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0 }, out[4 * 4 * 4];   /* c0 = 0xF800 red, c1 = 0x001F blue, all index 0 */
    CHECK(sr_tex_decode_bc1(blk, 4, 4, out) == 0, "bc1");
    for (int i = 0; i < 16; i++) CHECK(out[4*i] == 255 && out[4*i+1] == 0 && out[4*i+2] == 0 && out[4*i+3] == 255, "bc1 index 0 texel %d", i);
    blk[4] = 0x55;                                       /* texels 0..3 of row 0 -> index 1 = c1 (blue) */
    sr_tex_decode_bc1(blk, 4, 4, out);
    CHECK(out[0] == 0 && out[2] == 255, "bc1 index 1 blue: %d,%d,%d", out[0], out[1], out[2]);
    blk[4] = 0xAA;                                       /* index 2 = (2*c0 + c1) / 3 */
    sr_tex_decode_bc1(blk, 4, 4, out);
    CHECK(near1(out[0], 170) && near1(out[2], 85) && out[1] == 0, "bc1 index 2: %d,%d,%d", out[0], out[1], out[2]);
    uint8_t b3[16] = { 255, 0, 0, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0 };   /* alpha endpoints 255,0, all index 0 */
    CHECK(sr_tex_decode_bc3(b3, 4, 4, out) == 0, "bc3");
    for (int i = 0; i < 16; i++) CHECK(out[4*i+3] == 255 && out[4*i] == 255, "bc3 texel %d: a=%d r=%d", i, out[4*i+3], out[4*i]);
    return 1;
}

static double psnr(const uint8_t* a, const uint8_t* b, size_t n) {
    double e = 0; for (size_t i = 0; i < n; i++) { double d = (double)a[i] - b[i]; e += d * d; }
    e /= (double)n; return e == 0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / e);
}
static int bc7_case(const char* name, int kind, double min_db) {
    enum { W = 61, H = 45 };
    uint8_t* img = (uint8_t*)malloc(W * H * 4); uint8_t* back = (uint8_t*)malloc(((W + 3) / 4 * 4) * ((H + 3) / 4 * 4) * 4);
    uint8_t* blocks = (uint8_t*)malloc(((W + 3) / 4) * ((H + 3) / 4) * 16);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t* o = img + 4 * (y * W + x);
            if (kind == 0) { o[0] = (uint8_t)(x * 255 / (W - 1)); o[1] = (uint8_t)(y * 255 / (H - 1)); o[2] = (uint8_t)((x + y) * 255 / (W + H - 2)); o[3] = 255; }
            else if (kind == 1) { int c = ((x / 8) + (y / 8)) & 1; o[0] = o[1] = o[2] = c ? 230 : 25; o[3] = 255; }
            else { o[0] = (uint8_t)(128 + 100 * sin(x * 0.2) * cos(y * 0.15)); o[1] = (uint8_t)(x * 4); o[2] = (uint8_t)(255 - y * 5); o[3] = (uint8_t)(128 + 100 * sin((x + y) * 0.1)); }
        }
    int r = sr_tex_encode_bc7(img, W, H, 1, blocks);
    int ok = r == 0;
    if (ok) {
        ok = sr_tex_decode_bc7(blocks, W, H, back) == 0;
        if (ok) {
            /* decoder output is tightly packed W x H (cropped) */
            double db = psnr(img, back, W * H * 4);
            printf("  bc7 %s: PSNR %.2f dB\n", name, db);
            ok = db >= min_db;
            if (!ok) printf("  bc7 %s below %.1f dB\n", name, min_db);
        } else printf("  bc7 decode failed\n");
    } else printf("  bc7 encode rc %d\n", r);
    free(img); free(back); free(blocks);
    return ok;
}
static int t_bc7_quality(void) {
    CHECK(bc7_case("gradient", 0, 38.0), "gradient");
    CHECK(bc7_case("checker", 1, 28.0), "checker");
    CHECK(bc7_case("mixed alpha", 2, 30.0), "mixed");
    return 1;
}

/* ---- upload preparation ------------------------------------------------------------------------------------- */

static int t_prepare_upload(void) {
    /* 8x8 ASTC 4x4 texture, 2 mips (8x8 and 4x4), void-extent blocks of distinct colors, swizzled with bh=0 */
    sr_tex_surface_desc d; memset(&d, 0, sizeof d);
    d.format.fmt = SR_TEX_FMT_ASTC_4x4; d.format.srgb = 0; d.width = 8; d.height = 8; d.depth = 1; d.layers = 1; d.levels = 2; d.block_height_log2 = 0; d.block_depth_log2 = -1;
    sr_tex_plan p; CHECK(sr_tex_plan_build(&d, &p) == 0, "plan");
    uint8_t lin0[4 * 16], lin1[16];
    static const uint8_t cols[5][3] = { {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {16, 32, 64} };
    for (int i = 0; i < 4; i++) void_extent(lin0 + 16 * i, 257 * cols[i][0], 257 * cols[i][1], 257 * cols[i][2], 257 * 255);
    void_extent(lin1, 257 * cols[4][0], 257 * cols[4][1], 257 * cols[4][2], 257 * 255);
    uint8_t* guest = (uint8_t*)calloc(1, (size_t)p.total_size);
    CHECK(sr_tex_swizzle_level(&p, 0, 0, lin0, 0, 0, guest, (size_t)p.total_size) == 0 && sr_tex_swizzle_level(&p, 0, 1, lin1, 0, 0, guest, (size_t)p.total_size) == 0, "swizzle");
    sr_tex_upload up; sr_tex_gpu_caps caps = { 1, 1 };
    /* NATIVE: blocks pass through */
    CHECK(sr_tex_prepare_upload(&p, guest, (size_t)p.total_size, SR_TEX_TARGET_NATIVE, caps, &up) == 0, "native");
    CHECK(up.format.fmt == SR_TEX_FMT_ASTC_4x4 && up.region_count == 2 && up.data_size == 5 * 16, "native shape: fmt %d regions %u size %llu", up.format.fmt, up.region_count, (unsigned long long)up.data_size);
    CHECK(!memcmp(up.data, lin0, 64) && !memcmp(up.data + 64, lin1, 16), "native bytes");
    CHECK(up.regions[1].level == 1 && up.regions[1].width == 4 && up.regions[1].buffer_offset == 64, "native region 1");
    sr_tex_upload_free(&up);
    /* RGBA8 */
    CHECK(sr_tex_prepare_upload(&p, guest, (size_t)p.total_size, SR_TEX_TARGET_RGBA8, caps, &up) == 0, "rgba8");
    CHECK(up.format.fmt == SR_TEX_FMT_RGBA8 && up.error_blocks == 0 && up.data_size == (8 * 8 + 4 * 4) * 4, "rgba8 shape");
    for (int by = 0; by < 2; by++)
        for (int bx = 0; bx < 2; bx++) {
            const uint8_t* want = cols[by * 2 + bx]; const uint8_t* o = up.data + 4 * ((by * 4) * 8 + bx * 4);
            CHECK(o[0] == want[0] && o[1] == want[1] && o[2] == want[2] && o[3] == 255, "rgba8 block %d,%d = %d,%d,%d", bx, by, o[0], o[1], o[2]);
        }
    CHECK(up.regions[1].buffer_offset == 8 * 8 * 4 && up.data[8 * 8 * 4] == 16 && up.data[8 * 8 * 4 + 2] == 64, "rgba8 mip 1");
    sr_tex_upload_free(&up);
    /* BC7 target: flat blocks should survive almost exactly */
    CHECK(sr_tex_prepare_upload(&p, guest, (size_t)p.total_size, SR_TEX_TARGET_BC7, caps, &up) == 0, "bc7");
    CHECK(up.format.fmt == SR_TEX_FMT_BC7 && up.data_size == 5 * 16, "bc7 shape %llu", (unsigned long long)up.data_size);
    uint8_t px[8 * 8 * 4];
    CHECK(sr_tex_decode_bc7(up.data, 8, 8, px) == 0, "bc7 decode");
    for (int by = 0; by < 2; by++)
        for (int bx = 0; bx < 2; bx++) {
            const uint8_t* want = cols[by * 2 + bx]; const uint8_t* o = px + 4 * ((by * 4) * 8 + bx * 4);
            CHECK(abs(o[0] - want[0]) <= 3 && abs(o[1] - want[1]) <= 3 && abs(o[2] - want[2]) <= 3, "bc7 block %d,%d = %d,%d,%d", bx, by, o[0], o[1], o[2]);
        }
    sr_tex_upload_free(&up);
    /* AUTO picks by caps */
    sr_tex_gpu_caps a = { 1, 0 }, b = { 0, 1 }, c = { 0, 0 };
    CHECK(sr_tex_prepare_upload(&p, guest, (size_t)p.total_size, SR_TEX_TARGET_AUTO, a, &up) == 0 && up.format.fmt == SR_TEX_FMT_ASTC_4x4, "auto astc"); sr_tex_upload_free(&up);
    CHECK(sr_tex_prepare_upload(&p, guest, (size_t)p.total_size, SR_TEX_TARGET_AUTO, b, &up) == 0 && up.format.fmt == SR_TEX_FMT_BC7, "auto bc7"); sr_tex_upload_free(&up);
    CHECK(sr_tex_prepare_upload(&p, guest, (size_t)p.total_size, SR_TEX_TARGET_AUTO, c, &up) == 0 && up.format.fmt == SR_TEX_FMT_RGBA8, "auto rgba8"); sr_tex_upload_free(&up);
    /* guest too small */
    CHECK(sr_tex_prepare_upload(&p, guest, (size_t)p.total_size / 2, SR_TEX_TARGET_NATIVE, caps, &up) == SR_TEX_E_RANGE, "short guest");
    CHECK(sr_tex_prepare_upload(NULL, guest, 1, SR_TEX_TARGET_NATIVE, caps, &up) == SR_TEX_E_ARG, "null plan");
    free(guest);
    return 1;
}

static int t_prepare_upload_raw(void) {
    sr_tex_surface_desc d; memset(&d, 0, sizeof d);
    d.format.fmt = SR_TEX_FMT_RGB565; d.width = 4; d.height = 2; d.depth = 1; d.layers = 1; d.levels = 1; d.block_height_log2 = 0; d.block_depth_log2 = -1;
    sr_tex_plan p; CHECK(sr_tex_plan_build(&d, &p) == 0, "plan");
    uint8_t lin[4 * 2 * 2]; for (int i = 0; i < 8; i++) { lin[2*i] = 0x1F; lin[2*i+1] = 0xF8; }   /* 0xF81F: r=31 g=0 b=31 */
    uint8_t* guest = (uint8_t*)calloc(1, (size_t)p.total_size);
    CHECK(sr_tex_swizzle_level(&p, 0, 0, lin, 0, 0, guest, (size_t)p.total_size) == 0, "swizzle");
    sr_tex_upload up; sr_tex_gpu_caps caps = { 0, 1 };
    CHECK(sr_tex_prepare_upload(&p, guest, (size_t)p.total_size, SR_TEX_TARGET_AUTO, caps, &up) == 0, "auto raw");
    CHECK(up.format.fmt == SR_TEX_FMT_RGBA8 && up.data[0] == 255 && up.data[1] == 0 && up.data[2] == 255 && up.data[3] == 255, "565 -> rgba8 %d,%d,%d,%d", up.data[0], up.data[1], up.data[2], up.data[3]);
    sr_tex_upload_free(&up);
    CHECK(sr_tex_prepare_upload(&p, guest, (size_t)p.total_size, SR_TEX_TARGET_NATIVE, caps, &up) == 0 && up.format.fmt == SR_TEX_FMT_RGB565 && !memcmp(up.data, lin, sizeof lin), "native raw");
    sr_tex_upload_free(&up);
    free(guest);
    return 1;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
    RUN(t_gob_offset);
    RUN(t_default_bh);
    RUN(t_swizzle_model);
    RUN(t_roundtrip_formats);
    RUN(t_roundtrip_block_heights);
    RUN(t_roundtrip_arrays);
    RUN(t_roundtrip_3d);
    RUN(t_plan_invariants);
    RUN(t_range_and_args);
    RUN(t_unity_blob);
    RUN(t_unity_empty_blob);
    RUN(t_unity_cubemap);
    RUN(t_format_tables);
    RUN(t_astc_void_extent);
    RUN(t_astc_edges);
    RUN(t_astc_errors);
    RUN(t_bc1_bc3);
    RUN(t_bc7_quality);
    RUN(t_prepare_upload);
    RUN(t_prepare_upload_raw);
    printf("SUMMARY passed %d failed %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
