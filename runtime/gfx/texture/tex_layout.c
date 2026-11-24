/* Star Road Recomp texture path: Maxwell block-linear layout model, swizzle / deswizzle, Unity blob, upload
 * preparation. Our own code (D-008). The model is written from the hardware layout (GOB = 64 B x 8 rows with the
 * Maxwell bit interleave) and checked against every Texture2D in the owner's dump by tools/texture_check. */
#include "texture.h"
#include <stdlib.h>
#include <string.h>

static void* (*g_alloc)(size_t) = malloc;
static void (*g_free)(void*) = free;
void sr_tex_set_allocator(void* (*a)(size_t), void (*r)(void*)) { g_alloc = a ? a : malloc; g_free = r ? r : free; }
static void* tx_alloc(size_t n) { return g_alloc(n ? n : 1); }

static uint64_t cdiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }
static uint32_t cdiv32(uint32_t a, uint32_t b) { return (uint32_t)((a + b - 1) / b); }
static uint32_t maxu(uint32_t a, uint32_t b) { return a > b ? a : b; }

uint32_t sr_tex_default_bh_log2(uint32_t hb) {
    uint32_t hh = hb + hb / 2;
    return hh >= 128 ? 4 : hh >= 64 ? 3 : hh >= 32 ? 2 : hh >= 16 ? 1 : 0;
}

static uint32_t default_bd_log2(uint32_t d) {
    uint32_t l = 0;
    while ((1u << l) < d && l < 4) l++;
    return l;
}

/* Size in bytes of one swizzled mip for one layer. */
static uint64_t level_size(uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb, uint32_t bh_log2, uint32_t bd_log2) {
    uint64_t gobs_w = cdiv((uint64_t)wb * bpb, 64);
    uint64_t blocks_y = cdiv(hb, 8u << bh_log2);
    uint64_t blocks_z = cdiv(d, 1u << bd_log2);
    return gobs_w * blocks_y * blocks_z * (512ull << (bh_log2 + bd_log2));
}

int sr_tex_plan_build(const sr_tex_surface_desc* d, sr_tex_plan* p) {
    if (!d || !p) return SR_TEX_E_ARG;
    const sr_tex_format_info* fi = sr_tex_format_info_get(d->format.fmt);
    if (!fi) return SR_TEX_E_FORMAT;
    if (d->width == 0 || d->height == 0 || d->levels == 0 || d->levels > SR_TEX_MAX_LEVELS) return SR_TEX_E_ARG;
    memset(p, 0, sizeof *p);
    p->desc = *d;
    if (p->desc.layers == 0) p->desc.layers = 1;
    if (p->desc.dim != SR_TEX_DIM_3D || p->desc.depth == 0) p->desc.depth = 1;
    const uint32_t bw = fi->block_w, bh = fi->block_h, bpb = fi->bytes_per_block;
    uint32_t hb0 = cdiv32(d->height, bh);
    uint32_t bh0 = d->block_height_log2 >= 0 ? (uint32_t)d->block_height_log2 : sr_tex_default_bh_log2(hb0);
    uint32_t bd0 = 0;
    if (p->desc.dim == SR_TEX_DIM_3D) {
        bd0 = d->block_depth_log2 >= 0 ? (uint32_t)d->block_depth_log2 : default_bd_log2(p->desc.depth);
        if (d->block_height_log2 < 0 && bh0 + bd0 > 4) bh0 = 4 - bd0;   /* assumption: block <= 16 GOBs; no 3D textures in the dump */
    }
    if (bh0 > 5 || bd0 > 5) return SR_TEX_E_ARG;
    uint64_t off = 0, loff = 0;
    for (uint32_t m = 0; m < d->levels; m++) {
        sr_tex_level_plan* l = &p->level[m];
        l->width = maxu(d->width >> m, 1);
        l->height = maxu(d->height >> m, 1);
        l->depth = p->desc.dim == SR_TEX_DIM_3D ? maxu(p->desc.depth >> m, 1) : 1;
        l->blocks_w = cdiv32(l->width, bw);
        l->blocks_h = cdiv32(l->height, bh);
        uint32_t lbh = bh0, lbd = bd0;
        while (lbh > 0 && (8u << (lbh - 1)) >= l->blocks_h) lbh--;
        while (lbd > 0 && (1u << (lbd - 1)) >= l->depth) lbd--;
        l->bh_log2 = (uint8_t)lbh; l->bd_log2 = (uint8_t)lbd;
        l->offset = off;
        l->size = level_size(l->blocks_w, l->blocks_h, l->depth, bpb, lbh, lbd);
        off += l->size;
        l->linear_offset = loff;
        l->linear_size = (uint64_t)l->blocks_w * l->blocks_h * l->depth * bpb;
        loff += l->linear_size;
    }
    p->linear_layer_stride = loff;
    p->linear_total_size = loff * p->desc.layers;
    if (p->desc.layers > 1) {
        uint64_t al = 512ull << (bh0 + bd0);
        off = (off + al - 1) / al * al;
    }
    p->layer_stride = off;
    p->total_size = off * p->desc.layers;
    return SR_TEX_OK;
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int sr_tex_plan_from_unity_blob(int unity_format, int srgb, uint32_t width, uint32_t height, uint32_t mips,
                                const uint8_t* blob, size_t blob_size, uint64_t complete_image_size, sr_tex_plan* plan) {
    return sr_tex_plan_from_unity_blob_ex(unity_format, srgb, width, height, mips, 1, blob, blob_size, complete_image_size, plan);
}

int sr_tex_plan_from_unity_blob_ex(int unity_format, int srgb, uint32_t width, uint32_t height, uint32_t mips, uint32_t image_count,
                                   const uint8_t* blob, size_t blob_size, uint64_t complete_image_size, sr_tex_plan* plan) {
    if (!plan) return SR_TEX_E_ARG;
    if (image_count == 0 || image_count > 64) return SR_TEX_E_ARG;
    sr_tex_surface_desc d;
    memset(&d, 0, sizeof d);
    int r = sr_tex_format_from_unity(unity_format, srgb, &d.format);
    if (r) return r;
    /* Switch stores Unity RGB24 block-linear data as 4 bytes per texel (RGBX); the empty-blob linear form stays 3. */
    if (unity_format == 3 && blob_size >= 92) d.format.fmt = SR_TEX_FMT_RGBA8;
    if (blob_size == 0 && mips != 0 && mips <= SR_TEX_MAX_LEVELS) {
        /* Empty platform blob: the bytes are the tightly packed linear mip chain. */
        d.width = width; d.height = height; d.depth = 1; d.layers = image_count; d.levels = mips; d.dim = image_count == 6 ? SR_TEX_DIM_CUBE : SR_TEX_DIM_2D;
        d.block_height_log2 = 0; d.block_depth_log2 = -1;
        r = sr_tex_plan_build(&d, plan);
        if (r) return r;
        for (uint32_t m = 0; m < mips; m++) { plan->level[m].offset = plan->level[m].linear_offset; plan->level[m].size = plan->level[m].linear_size; }
        plan->layer_stride = plan->linear_layer_stride; plan->total_size = plan->linear_total_size; plan->stored_linear = 1;
        return (complete_image_size && complete_image_size != plan->layer_stride) ? SR_TEX_E_MISMATCH : SR_TEX_OK;
    }
    if (!blob || blob_size < 92 || mips == 0 || mips > SR_TEX_MAX_LEVELS) return SR_TEX_E_BLOB;
    uint32_t bhl = rd32(blob + 8);
    if (bhl > 5) return SR_TEX_E_BLOB;
    d.width = width; d.height = height; d.depth = 1; d.layers = image_count; d.levels = mips; d.dim = image_count == 6 ? SR_TEX_DIM_CUBE : SR_TEX_DIM_2D;
    d.block_height_log2 = (int32_t)bhl; d.block_depth_log2 = -1;
    r = sr_tex_plan_build(&d, plan);
    if (r) return r;
    int bad = 0;
    for (uint32_t m = 0; m < mips; m++)
        if (rd32(blob + 16 + 4 * m) != plan->level[m].offset) bad = 1;
    uint64_t total = rd32(blob + 84);
    if (total != plan->layer_stride) bad = 1;      /* Unity sizes describe one image; layer_stride == total_size for a plain 2D texture */
    if (complete_image_size && complete_image_size != plan->layer_stride) bad = 1;
    return bad ? SR_TEX_E_MISMATCH : SR_TEX_OK;
}

/* ---- swizzle ------------------------------------------------------------------------------------------- */

static int level_check(const sr_tex_plan* p, uint32_t layer, uint32_t level, size_t swz_size, uint64_t* base) {
    if (!p || level >= p->desc.levels || layer >= p->desc.layers) return SR_TEX_E_ARG;
    const sr_tex_level_plan* l = &p->level[level];
    uint64_t b = (uint64_t)layer * p->layer_stride + l->offset;
    if (b + l->size > swz_size) return SR_TEX_E_RANGE;
    *base = b;
    return SR_TEX_OK;
}

/* Copy one level between swizzled and linear. to_linear selects direction. */
static int walk(const sr_tex_plan* p, uint32_t layer, uint32_t level, uint8_t* swz, size_t swz_size,
                uint8_t* lin, uint64_t row_pitch, uint64_t slice_pitch, int to_linear) {
    uint64_t base;
    int r = level_check(p, layer, level, swz_size, &base);
    if (r) return r;
    const sr_tex_level_plan* l = &p->level[level];
    const sr_tex_format_info* fi = sr_tex_format_info_get(p->desc.format.fmt);
    const uint64_t rowb = (uint64_t)l->blocks_w * fi->bytes_per_block;
    if (!row_pitch) row_pitch = rowb;
    if (!slice_pitch) slice_pitch = row_pitch * l->blocks_h;
    if (row_pitch < rowb) return SR_TEX_E_ARG;
    const uint64_t gobs_w = cdiv(rowb, 64);
    const uint32_t bh = 1u << l->bh_log2, bd = 1u << l->bd_log2;
    const uint64_t blocks_y = cdiv(l->blocks_h, 8u * bh);
    const uint64_t block_bytes = 512ull * bh * bd;
    if (p->stored_linear) {
        for (uint32_t z = 0; z < l->depth; z++)
            for (uint32_t y = 0; y < l->blocks_h; y++) {
                uint8_t* s = swz + base + ((uint64_t)z * l->blocks_h + y) * rowb;
                uint8_t* lrow = lin + (uint64_t)z * slice_pitch + (uint64_t)y * row_pitch;
                if (to_linear) memcpy(lrow, s, (size_t)rowb); else memcpy(s, lrow, (size_t)rowb);
            }
        return SR_TEX_OK;
    }
    if (!to_linear) memset(swz + base, 0, (size_t)l->size);
    for (uint32_t z = 0; z < l->depth; z++) {
        const uint64_t zb = z / bd, zin = z % bd;
        for (uint32_t y = 0; y < l->blocks_h; y++) {
            const uint64_t yb = y / (8u * bh), yin = (y / 8) % bh, ry = y & 7;
            const uint64_t row_base = base + ((zb * blocks_y + yb) * gobs_w) * block_bytes + (zin * bh + yin) * 512;
            const uint32_t ybits = (uint32_t)(((ry >> 1) & 3) << 6 | (ry & 1) << 4);
            uint8_t* lrow = lin + (uint64_t)z * slice_pitch + (uint64_t)y * row_pitch;
            for (uint64_t gx = 0; gx < gobs_w; gx++) {
                uint8_t* gob = swz + row_base + gx * block_bytes;
                uint64_t x0 = gx * 64, rem = rowb - x0;
                if (rem >= 64) {
                    for (uint32_t c = 0; c < 4; c++) {
                        uint8_t* s = gob + ybits + ((c >> 1) << 8) + ((c & 1) << 5);
                        if (to_linear) memcpy(lrow + x0 + c * 16, s, 16); else memcpy(s, lrow + x0 + c * 16, 16);
                    }
                } else {
                    for (uint32_t c = 0; c < 4 && (uint64_t)c * 16 < rem; c++) {
                        uint8_t* s = gob + ybits + ((c >> 1) << 8) + ((c & 1) << 5);
                        size_t n = rem - (uint64_t)c * 16 < 16 ? (size_t)(rem - (uint64_t)c * 16) : 16;
                        if (to_linear) memcpy(lrow + x0 + c * 16, s, n); else memcpy(s, lrow + x0 + c * 16, n);
                    }
                }
            }
        }
    }
    return SR_TEX_OK;
}

int sr_tex_deswizzle_level(const sr_tex_plan* plan, uint32_t layer, uint32_t level, const uint8_t* swz, size_t swz_size,
                           uint8_t* lin, uint64_t row_pitch, uint64_t slice_pitch) {
    if (!swz || !lin) return SR_TEX_E_ARG;
    return walk(plan, layer, level, (uint8_t*)(uintptr_t)swz, swz_size, lin, row_pitch, slice_pitch, 1);
}

int sr_tex_swizzle_level(const sr_tex_plan* plan, uint32_t layer, uint32_t level, const uint8_t* lin, uint64_t row_pitch,
                         uint64_t slice_pitch, uint8_t* swz, size_t swz_size) {
    if (!swz || !lin) return SR_TEX_E_ARG;
    return walk(plan, layer, level, swz, swz_size, (uint8_t*)(uintptr_t)lin, row_pitch, slice_pitch, 0);
}

/* ---- upload -------------------------------------------------------------------------------------------- */

typedef enum { CONV_NONE, CONV_ASTC_RGBA8, CONV_ASTC_BC7, CONV_RAW_RGBA8 } conv_t;

static int raw_needs_rgba8(sr_tex_fmt f) {
    switch (f) {
    case SR_TEX_FMT_RGB8: case SR_TEX_FMT_ARGB4444: case SR_TEX_FMT_ARGB8: case SR_TEX_FMT_RGBA4:
    case SR_TEX_FMT_RGB5A1: case SR_TEX_FMT_BGR5A1: case SR_TEX_FMT_RGB565: return 1;
    default: return 0;
    }
}

void sr_tex_upload_free(sr_tex_upload* up) {
    if (!up) return;
    if (up->regions) g_free(up->regions);
    if (up->data) g_free(up->data);
    memset(up, 0, sizeof *up);
}

int sr_tex_prepare_upload(const sr_tex_plan* plan, const uint8_t* guest, size_t guest_size, sr_tex_target target,
                          sr_tex_gpu_caps caps, sr_tex_upload* out) {
    if (!plan || !guest || !out) return SR_TEX_E_ARG;
    memset(out, 0, sizeof *out);
    const sr_tex_format_info* fi = sr_tex_format_info_get(plan->desc.format.fmt);
    if (!fi) return SR_TEX_E_FORMAT;
    if (target == SR_TEX_TARGET_AUTO) {
        if (fi->codec == SR_TEX_CODEC_ASTC) target = caps.astc ? SR_TEX_TARGET_NATIVE : caps.bc7 ? SR_TEX_TARGET_BC7 : SR_TEX_TARGET_RGBA8;
        else target = raw_needs_rgba8(plan->desc.format.fmt) ? SR_TEX_TARGET_RGBA8 : SR_TEX_TARGET_NATIVE;
    }
    conv_t cv = CONV_NONE;
    sr_tex_format ofmt = plan->desc.format;
    if (fi->codec == SR_TEX_CODEC_ASTC && target == SR_TEX_TARGET_RGBA8) { cv = CONV_ASTC_RGBA8; ofmt.fmt = SR_TEX_FMT_RGBA8; }
    else if (fi->codec == SR_TEX_CODEC_ASTC && target == SR_TEX_TARGET_BC7) { cv = CONV_ASTC_BC7; ofmt.fmt = SR_TEX_FMT_BC7; }
    else if (fi->codec == SR_TEX_CODEC_RAW && target != SR_TEX_TARGET_NATIVE && raw_needs_rgba8(plan->desc.format.fmt)) {
        cv = CONV_RAW_RGBA8; ofmt.fmt = SR_TEX_FMT_RGBA8; ofmt.srgb = plan->desc.format.srgb;
    }
    const uint32_t nl = plan->desc.levels, nlay = plan->desc.layers;
    const sr_tex_format_info* ofi = sr_tex_format_info_get(ofmt.fmt);
    out->regions = (sr_tex_region*)tx_alloc(sizeof(sr_tex_region) * nl * nlay);
    if (!out->regions) return SR_TEX_E_NOMEM;
    uint64_t total = 0, maxlin = 0;
    for (uint32_t lay = 0; lay < nlay; lay++)
        for (uint32_t m = 0; m < nl; m++) {
            const sr_tex_level_plan* l = &plan->level[m];
            sr_tex_region* g = &out->regions[lay * nl + m];
            g->level = m; g->layer = lay; g->width = l->width; g->height = l->height; g->depth = l->depth;
            uint32_t obw = cdiv32(l->width, ofi->block_w), obh = cdiv32(l->height, ofi->block_h);
            g->buffer_row_length = obw * ofi->block_w;
            g->buffer_image_height = obh * ofi->block_h;
            g->byte_size = (uint64_t)obw * obh * l->depth * ofi->bytes_per_block;
            g->buffer_offset = total;
            total += g->byte_size;
            if (l->linear_size > maxlin) maxlin = l->linear_size;
        }
    out->region_count = nl * nlay;
    out->data_size = total;
    out->data = (uint8_t*)tx_alloc((size_t)total);
    uint8_t* lin = (uint8_t*)tx_alloc((size_t)maxlin);
    uint8_t* rgba = NULL;
    if (!out->data || !lin) { g_free(lin); sr_tex_upload_free(out); return SR_TEX_E_NOMEM; }
    int status = SR_TEX_OK;
    for (uint32_t lay = 0; lay < nlay && status >= 0; lay++)
        for (uint32_t m = 0; m < nl && status >= 0; m++) {
            const sr_tex_level_plan* l = &plan->level[m];
            const sr_tex_region* g = &out->regions[lay * nl + m];
            uint8_t* dst = out->data + g->buffer_offset;
            if (cv == CONV_NONE) {
                status = sr_tex_deswizzle_level(plan, lay, m, guest, guest_size, dst, 0, 0);
                continue;
            }
            status = sr_tex_deswizzle_level(plan, lay, m, guest, guest_size, lin, 0, 0);
            if (status) break;
            const uint64_t slice_in = (uint64_t)l->blocks_w * l->blocks_h * fi->bytes_per_block;
            for (uint32_t z = 0; z < l->depth && status >= 0; z++) {
                const uint8_t* src = lin + z * slice_in;
                if (cv == CONV_ASTC_RGBA8) {
                    uint32_t eb = 0;
                    sr_tex_decode_astc_ldr(src, fi->block_w, fi->block_h, l->width, l->height, plan->desc.format.srgb, dst + (uint64_t)z * l->width * l->height * 4, &eb);
                    out->error_blocks += eb;
                } else if (cv == CONV_ASTC_BC7) {
                    uint32_t eb = 0;
                    rgba = (uint8_t*)tx_alloc((size_t)l->width * l->height * 4);
                    if (!rgba) { status = SR_TEX_E_NOMEM; break; }
                    sr_tex_decode_astc_ldr(src, fi->block_w, fi->block_h, l->width, l->height, plan->desc.format.srgb, rgba, &eb);
                    out->error_blocks += eb;
                    uint64_t zsz = (uint64_t)cdiv32(l->width, 4) * cdiv32(l->height, 4) * 16;
                    status = sr_tex_encode_bc7(rgba, l->width, l->height, 1, dst + z * zsz);
                    g_free(rgba); rgba = NULL;
                } else {
                    status = sr_tex_raw_to_rgba8(plan->desc.format.fmt, src, l->width, l->height, dst + (uint64_t)z * l->width * l->height * 4);
                }
            }
        }
    g_free(lin);
    if (status < 0) { sr_tex_upload_free(out); return status; }
    out->format = ofmt;
    return SR_TEX_OK;
}

/* ---- raw previews --------------------------------------------------------------------------------------- */

int sr_tex_raw_to_rgba8(sr_tex_fmt f, const uint8_t* s, uint32_t w, uint32_t h, uint8_t* o) {
    if (!s || !o) return SR_TEX_E_ARG;
    size_t n = (size_t)w * h;
    switch (f) {
    case SR_TEX_FMT_R8: for (size_t i = 0; i < n; i++) { o[4*i] = s[i]; o[4*i+1] = s[i]; o[4*i+2] = s[i]; o[4*i+3] = 255; } return 0;
    case SR_TEX_FMT_A8: for (size_t i = 0; i < n; i++) { o[4*i] = 255; o[4*i+1] = 255; o[4*i+2] = 255; o[4*i+3] = s[i]; } return 0;
    case SR_TEX_FMT_RG8: for (size_t i = 0; i < n; i++) { o[4*i] = s[2*i]; o[4*i+1] = s[2*i+1]; o[4*i+2] = 0; o[4*i+3] = 255; } return 0;
    case SR_TEX_FMT_RGB8: for (size_t i = 0; i < n; i++) { o[4*i] = s[3*i]; o[4*i+1] = s[3*i+1]; o[4*i+2] = s[3*i+2]; o[4*i+3] = 255; } return 0;
    case SR_TEX_FMT_RGBA8: memcpy(o, s, n * 4); return 0;
    case SR_TEX_FMT_BGRA8: for (size_t i = 0; i < n; i++) { o[4*i] = s[4*i+2]; o[4*i+1] = s[4*i+1]; o[4*i+2] = s[4*i]; o[4*i+3] = s[4*i+3]; } return 0;
    case SR_TEX_FMT_ARGB8: for (size_t i = 0; i < n; i++) { o[4*i] = s[4*i+1]; o[4*i+1] = s[4*i+2]; o[4*i+2] = s[4*i+3]; o[4*i+3] = s[4*i]; } return 0;
    case SR_TEX_FMT_ARGB4444:   /* Unity ARGB4444 little-endian 16-bit: A in bits 15..12, R 11..8, G 7..4, B 3..0 */
        for (size_t i = 0; i < n; i++) { unsigned v = s[2*i] | s[2*i+1] << 8; o[4*i] = ((v >> 8) & 15) * 17; o[4*i+1] = ((v >> 4) & 15) * 17; o[4*i+2] = (v & 15) * 17; o[4*i+3] = ((v >> 12) & 15) * 17; } return 0;
    case SR_TEX_FMT_RGBA4:      /* R 15..12, G 11..8, B 7..4, A 3..0 */
        for (size_t i = 0; i < n; i++) { unsigned v = s[2*i] | s[2*i+1] << 8; o[4*i] = ((v >> 12) & 15) * 17; o[4*i+1] = ((v >> 8) & 15) * 17; o[4*i+2] = ((v >> 4) & 15) * 17; o[4*i+3] = (v & 15) * 17; } return 0;
    case SR_TEX_FMT_RGB565:
        for (size_t i = 0; i < n; i++) { unsigned v = s[2*i] | s[2*i+1] << 8; unsigned r = v >> 11, g = (v >> 5) & 63, b = v & 31; o[4*i] = (uint8_t)((r << 3) | (r >> 2)); o[4*i+1] = (uint8_t)((g << 2) | (g >> 4)); o[4*i+2] = (uint8_t)((b << 3) | (b >> 2)); o[4*i+3] = 255; } return 0;
    case SR_TEX_FMT_RGB5A1:     /* R 15..11, G 10..6, B 5..1, A 0 */
        for (size_t i = 0; i < n; i++) { unsigned v = s[2*i] | s[2*i+1] << 8; unsigned r = v >> 11, g = (v >> 6) & 31, b = (v >> 1) & 31; o[4*i] = (uint8_t)((r << 3) | (r >> 2)); o[4*i+1] = (uint8_t)((g << 3) | (g >> 2)); o[4*i+2] = (uint8_t)((b << 3) | (b >> 2)); o[4*i+3] = (v & 1) ? 255 : 0; } return 0;
    case SR_TEX_FMT_BGR5A1:     /* B 15..11, G 10..6, R 5..1, A 0 */
        for (size_t i = 0; i < n; i++) { unsigned v = s[2*i] | s[2*i+1] << 8; unsigned b = v >> 11, g = (v >> 6) & 31, r = (v >> 1) & 31; o[4*i] = (uint8_t)((r << 3) | (r >> 2)); o[4*i+1] = (uint8_t)((g << 3) | (g >> 2)); o[4*i+2] = (uint8_t)((b << 3) | (b >> 2)); o[4*i+3] = (v & 1) ? 255 : 0; } return 0;
    default: return SR_TEX_E_FORMAT;
    }
}
