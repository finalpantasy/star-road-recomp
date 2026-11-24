/* Star Road Recomp texture path: format tables and NVN / Unity id mapping. Our own code.
 * NVN Format values follow the published ordering (R8 at 0x01, RGBA8 at 0x25, BC at 0x41.., ASTC at 0x79..);
 * lessons from other recomps confirm the group layout, the game's own use is re-checked by task-12. */
#include "texture.h"
#include <string.h>

#define F(id, nm, codec, bw, bh, bpb, ch, ds) [SR_TEX_FMT_##id] = { nm, SR_TEX_CODEC_##codec, bw, bh, bpb, ch, ds }
static const sr_tex_format_info g_info[SR_TEX_FMT__COUNT] = {
    F(R8, "R8", RAW, 1, 1, 1, 1, 0), F(R8_SNORM, "R8_SNORM", RAW, 1, 1, 1, 1, 0), F(R8_UINT, "R8_UINT", RAW, 1, 1, 1, 1, 0),
    F(R8_SINT, "R8_SINT", RAW, 1, 1, 1, 1, 0), F(A8, "A8", RAW, 1, 1, 1, 1, 0),
    F(R16_FLOAT, "R16_FLOAT", RAW, 1, 1, 2, 1, 0), F(R16, "R16", RAW, 1, 1, 2, 1, 0), F(R16_SNORM, "R16_SNORM", RAW, 1, 1, 2, 1, 0),
    F(R16_UINT, "R16_UINT", RAW, 1, 1, 2, 1, 0), F(R16_SINT, "R16_SINT", RAW, 1, 1, 2, 1, 0),
    F(RG8, "RG8", RAW, 1, 1, 2, 2, 0), F(RG8_SNORM, "RG8_SNORM", RAW, 1, 1, 2, 2, 0), F(RG8_UINT, "RG8_UINT", RAW, 1, 1, 2, 2, 0),
    F(RG8_SINT, "RG8_SINT", RAW, 1, 1, 2, 2, 0),
    F(RGBA4, "RGBA4", RAW, 1, 1, 2, 4, 0), F(ARGB4444, "ARGB4444", RAW, 1, 1, 2, 4, 0), F(RGB5A1, "RGB5A1", RAW, 1, 1, 2, 4, 0),
    F(RGB565, "RGB565", RAW, 1, 1, 2, 3, 0), F(BGR5A1, "BGR5A1", RAW, 1, 1, 2, 4, 0),
    F(RGB8, "RGB8", RAW, 1, 1, 3, 3, 0),
    F(R32_FLOAT, "R32_FLOAT", RAW, 1, 1, 4, 1, 0), F(R32_UINT, "R32_UINT", RAW, 1, 1, 4, 1, 0), F(R32_SINT, "R32_SINT", RAW, 1, 1, 4, 1, 0),
    F(RG16_FLOAT, "RG16_FLOAT", RAW, 1, 1, 4, 2, 0), F(RG16, "RG16", RAW, 1, 1, 4, 2, 0), F(RG16_SNORM, "RG16_SNORM", RAW, 1, 1, 4, 2, 0),
    F(RG16_UINT, "RG16_UINT", RAW, 1, 1, 4, 2, 0), F(RG16_SINT, "RG16_SINT", RAW, 1, 1, 4, 2, 0),
    F(RGBA8, "RGBA8", RAW, 1, 1, 4, 4, 0), F(RGBA8_SNORM, "RGBA8_SNORM", RAW, 1, 1, 4, 4, 0), F(RGBA8_UINT, "RGBA8_UINT", RAW, 1, 1, 4, 4, 0),
    F(RGBA8_SINT, "RGBA8_SINT", RAW, 1, 1, 4, 4, 0), F(BGRA8, "BGRA8", RAW, 1, 1, 4, 4, 0), F(ARGB8, "ARGB8", RAW, 1, 1, 4, 4, 0),
    F(RGB10A2, "RGB10A2", RAW, 1, 1, 4, 4, 0), F(RGB10A2_UINT, "RGB10A2_UINT", RAW, 1, 1, 4, 4, 0),
    F(R11G11B10_FLOAT, "R11G11B10_FLOAT", RAW, 1, 1, 4, 3, 0), F(RGB9E5, "RGB9E5", RAW, 1, 1, 4, 3, 0),
    F(RGB16_FLOAT, "RGB16_FLOAT", RAW, 1, 1, 6, 3, 0),
    F(RG32_FLOAT, "RG32_FLOAT", RAW, 1, 1, 8, 2, 0), F(RG32_UINT, "RG32_UINT", RAW, 1, 1, 8, 2, 0), F(RG32_SINT, "RG32_SINT", RAW, 1, 1, 8, 2, 0),
    F(RGBA16_FLOAT, "RGBA16_FLOAT", RAW, 1, 1, 8, 4, 0), F(RGBA16, "RGBA16", RAW, 1, 1, 8, 4, 0), F(RGBA16_SNORM, "RGBA16_SNORM", RAW, 1, 1, 8, 4, 0),
    F(RGBA16_UINT, "RGBA16_UINT", RAW, 1, 1, 8, 4, 0), F(RGBA16_SINT, "RGBA16_SINT", RAW, 1, 1, 8, 4, 0),
    F(RGB32_FLOAT, "RGB32_FLOAT", RAW, 1, 1, 12, 3, 0),
    F(RGBA32_FLOAT, "RGBA32_FLOAT", RAW, 1, 1, 16, 4, 0), F(RGBA32_UINT, "RGBA32_UINT", RAW, 1, 1, 16, 4, 0), F(RGBA32_SINT, "RGBA32_SINT", RAW, 1, 1, 16, 4, 0),
    F(STENCIL8, "STENCIL8", RAW, 1, 1, 1, 0, 1), F(DEPTH16, "DEPTH16", RAW, 1, 1, 2, 0, 1), F(DEPTH24, "DEPTH24", RAW, 1, 1, 4, 0, 1),
    F(DEPTH32_FLOAT, "DEPTH32_FLOAT", RAW, 1, 1, 4, 0, 1), F(DEPTH24_STENCIL8, "DEPTH24_STENCIL8", RAW, 1, 1, 4, 0, 1),
    F(DEPTH32_FLOAT_STENCIL8, "DEPTH32_FLOAT_STENCIL8", RAW, 1, 1, 8, 0, 1),
    F(BC1_RGB, "BC1_RGB", BC1, 4, 4, 8, 4, 0), F(BC1_RGBA, "BC1_RGBA", BC1, 4, 4, 8, 4, 0), F(BC2, "BC2", BC2, 4, 4, 16, 4, 0),
    F(BC3, "BC3", BC3, 4, 4, 16, 4, 0), F(BC4, "BC4", BC4, 4, 4, 8, 1, 0), F(BC4_SNORM, "BC4_SNORM", BC4, 4, 4, 8, 1, 0),
    F(BC5, "BC5", BC5, 4, 4, 16, 2, 0), F(BC5_SNORM, "BC5_SNORM", BC5, 4, 4, 16, 2, 0),
    F(BC6H_UFLOAT, "BC6H_UFLOAT", BC6H, 4, 4, 16, 3, 0), F(BC6H_SFLOAT, "BC6H_SFLOAT", BC6H, 4, 4, 16, 3, 0), F(BC7, "BC7", BC7, 4, 4, 16, 4, 0),
    F(ASTC_4x4, "ASTC_4x4", ASTC, 4, 4, 16, 4, 0), F(ASTC_5x4, "ASTC_5x4", ASTC, 5, 4, 16, 4, 0), F(ASTC_5x5, "ASTC_5x5", ASTC, 5, 5, 16, 4, 0),
    F(ASTC_6x5, "ASTC_6x5", ASTC, 6, 5, 16, 4, 0), F(ASTC_6x6, "ASTC_6x6", ASTC, 6, 6, 16, 4, 0), F(ASTC_8x5, "ASTC_8x5", ASTC, 8, 5, 16, 4, 0),
    F(ASTC_8x6, "ASTC_8x6", ASTC, 8, 6, 16, 4, 0), F(ASTC_8x8, "ASTC_8x8", ASTC, 8, 8, 16, 4, 0), F(ASTC_10x5, "ASTC_10x5", ASTC, 10, 5, 16, 4, 0),
    F(ASTC_10x6, "ASTC_10x6", ASTC, 10, 6, 16, 4, 0), F(ASTC_10x8, "ASTC_10x8", ASTC, 10, 8, 16, 4, 0), F(ASTC_10x10, "ASTC_10x10", ASTC, 10, 10, 16, 4, 0),
    F(ASTC_12x10, "ASTC_12x10", ASTC, 12, 10, 16, 4, 0), F(ASTC_12x12, "ASTC_12x12", ASTC, 12, 12, 16, 4, 0),
};
#undef F

const sr_tex_format_info* sr_tex_format_info_get(sr_tex_fmt fmt) {
    if ((int)fmt <= 0 || (int)fmt >= SR_TEX_FMT__COUNT) return NULL;
    return g_info[fmt].name ? &g_info[fmt] : NULL;
}

static int set(sr_tex_format* o, sr_tex_fmt f, int srgb) { o->fmt = f; o->srgb = (uint8_t)(srgb ? 1 : 0); return SR_TEX_OK; }

int sr_tex_format_from_nvn(uint32_t n, sr_tex_format* out) {
    if (!out) return SR_TEX_E_ARG;
    static const sr_tex_fmt lo[0x25] = { /* 0x00 .. 0x24 */
        SR_TEX_FMT_NONE,
        SR_TEX_FMT_R8, SR_TEX_FMT_R8_SNORM, SR_TEX_FMT_R8_UINT, SR_TEX_FMT_R8_SINT,
        SR_TEX_FMT_R16_FLOAT, SR_TEX_FMT_R16, SR_TEX_FMT_R16_SNORM, SR_TEX_FMT_R16_UINT, SR_TEX_FMT_R16_SINT,
        SR_TEX_FMT_R32_FLOAT, SR_TEX_FMT_R32_UINT, SR_TEX_FMT_R32_SINT,
        SR_TEX_FMT_RG8, SR_TEX_FMT_RG8_SNORM, SR_TEX_FMT_RG8_UINT, SR_TEX_FMT_RG8_SINT,
        SR_TEX_FMT_RG16_FLOAT, SR_TEX_FMT_RG16, SR_TEX_FMT_RG16_SNORM, SR_TEX_FMT_RG16_UINT, SR_TEX_FMT_RG16_SINT,
        SR_TEX_FMT_RG32_FLOAT, SR_TEX_FMT_RG32_UINT, SR_TEX_FMT_RG32_SINT,
        SR_TEX_FMT_NONE /*0x19 RGB8*/, SR_TEX_FMT_NONE, SR_TEX_FMT_NONE, SR_TEX_FMT_NONE,
        SR_TEX_FMT_NONE /*0x1D RGB16F*/, SR_TEX_FMT_NONE, SR_TEX_FMT_NONE, SR_TEX_FMT_NONE, SR_TEX_FMT_NONE,
        SR_TEX_FMT_NONE /*0x22 RGB32F*/, SR_TEX_FMT_NONE, SR_TEX_FMT_NONE };
    if (n < 0x25) {
        if (n == 0x19 || n == 0x1A || n == 0x1B || n == 0x1C) return set(out, SR_TEX_FMT_RGB8, 0);
        if (n == 0x1D) return set(out, SR_TEX_FMT_RGB16_FLOAT, 0);
        if (n == 0x22) return set(out, SR_TEX_FMT_RGB32_FLOAT, 0);
        if (lo[n] == SR_TEX_FMT_NONE) return SR_TEX_E_FORMAT;
        return set(out, lo[n], 0);
    }
    switch (n) {
    case 0x25: return set(out, SR_TEX_FMT_RGBA8, 0);
    case 0x26: return set(out, SR_TEX_FMT_RGBA8_SNORM, 0);
    case 0x27: return set(out, SR_TEX_FMT_RGBA8_UINT, 0);
    case 0x28: return set(out, SR_TEX_FMT_RGBA8_SINT, 0);
    case 0x29: return set(out, SR_TEX_FMT_RGBA16_FLOAT, 0);
    case 0x2A: return set(out, SR_TEX_FMT_RGBA16, 0);
    case 0x2B: return set(out, SR_TEX_FMT_RGBA16_SNORM, 0);
    case 0x2C: return set(out, SR_TEX_FMT_RGBA16_UINT, 0);
    case 0x2D: return set(out, SR_TEX_FMT_RGBA16_SINT, 0);
    case 0x2E: return set(out, SR_TEX_FMT_RGBA32_FLOAT, 0);
    case 0x2F: return set(out, SR_TEX_FMT_RGBA32_UINT, 0);
    case 0x30: return set(out, SR_TEX_FMT_RGBA32_SINT, 0);
    case 0x31: return set(out, SR_TEX_FMT_STENCIL8, 0);
    case 0x32: return set(out, SR_TEX_FMT_DEPTH16, 0);
    case 0x33: return set(out, SR_TEX_FMT_DEPTH24, 0);
    case 0x34: return set(out, SR_TEX_FMT_DEPTH32_FLOAT, 0);
    case 0x35: return set(out, SR_TEX_FMT_DEPTH24_STENCIL8, 0);
    case 0x36: return set(out, SR_TEX_FMT_DEPTH32_FLOAT_STENCIL8, 0);
    case 0x37: case 0x38: return set(out, SR_TEX_FMT_RGBA8, 1);       /* RGBX8_SRGB, RGBA8_SRGB */
    case 0x39: return set(out, SR_TEX_FMT_RGBA4, 0);
    case 0x3A: return set(out, SR_TEX_FMT_RGB5A1, 0);                  /* RGB5 (X1) */
    case 0x3B: return set(out, SR_TEX_FMT_RGB5A1, 0);
    case 0x3C: return set(out, SR_TEX_FMT_RGB565, 0);
    case 0x3D: return set(out, SR_TEX_FMT_RGB10A2, 0);
    case 0x3E: return set(out, SR_TEX_FMT_RGB10A2_UINT, 0);
    case 0x3F: return set(out, SR_TEX_FMT_R11G11B10_FLOAT, 0);
    case 0x40: return set(out, SR_TEX_FMT_RGB9E5, 0);
    case 0x41: return set(out, SR_TEX_FMT_BC1_RGB, 0);
    case 0x42: return set(out, SR_TEX_FMT_BC1_RGBA, 0);
    case 0x43: return set(out, SR_TEX_FMT_BC2, 0);
    case 0x44: return set(out, SR_TEX_FMT_BC3, 0);
    case 0x45: return set(out, SR_TEX_FMT_BC1_RGB, 1);
    case 0x46: return set(out, SR_TEX_FMT_BC1_RGBA, 1);
    case 0x47: return set(out, SR_TEX_FMT_BC2, 1);
    case 0x48: return set(out, SR_TEX_FMT_BC3, 1);
    case 0x49: return set(out, SR_TEX_FMT_BC4, 0);
    case 0x4A: return set(out, SR_TEX_FMT_BC4_SNORM, 0);
    case 0x4B: return set(out, SR_TEX_FMT_BC5, 0);
    case 0x4C: return set(out, SR_TEX_FMT_BC5_SNORM, 0);
    case 0x4D: return set(out, SR_TEX_FMT_BC7, 0);
    case 0x4E: return set(out, SR_TEX_FMT_BC7, 1);
    case 0x4F: return set(out, SR_TEX_FMT_BC6H_SFLOAT, 0);
    case 0x50: return set(out, SR_TEX_FMT_BC6H_UFLOAT, 0);
    case 0x95: case 0x96: case 0x97: return set(out, SR_TEX_FMT_BGR5A1, 0);
    case 0x98: case 0x99: return set(out, SR_TEX_FMT_BGRA8, 0);
    case 0x9A: return set(out, SR_TEX_FMT_BGRA8, 0);
    case 0x9B: case 0x9C: return set(out, SR_TEX_FMT_BGRA8, 1);
    default: break;
    }
    if (n >= 0x79 && n <= 0x86) return set(out, (sr_tex_fmt)(SR_TEX_FMT_ASTC_4x4 + (n - 0x79)), 0);
    if (n >= 0x87 && n <= 0x94) return set(out, (sr_tex_fmt)(SR_TEX_FMT_ASTC_4x4 + (n - 0x87)), 1);
    return SR_TEX_E_FORMAT;
}

int sr_tex_format_from_unity(int tf, int srgb, sr_tex_format* out) {
    if (!out) return SR_TEX_E_ARG;
    switch (tf) {
    case 1: return set(out, SR_TEX_FMT_A8, 0);
    case 2: return set(out, SR_TEX_FMT_ARGB4444, 0);
    case 3: return set(out, SR_TEX_FMT_RGB8, 0);
    case 4: return set(out, SR_TEX_FMT_RGBA8, srgb);
    case 5: return set(out, SR_TEX_FMT_ARGB8, srgb);
    case 7: return set(out, SR_TEX_FMT_RGB565, 0);
    case 9: return set(out, SR_TEX_FMT_R16, 0);
    case 10: return set(out, SR_TEX_FMT_BC1_RGB, srgb);
    case 12: return set(out, SR_TEX_FMT_BC3, srgb);
    case 13: return set(out, SR_TEX_FMT_RGBA4, 0);
    case 14: return set(out, SR_TEX_FMT_BGRA8, srgb);
    case 15: return set(out, SR_TEX_FMT_R16_FLOAT, 0);
    case 16: return set(out, SR_TEX_FMT_RG16_FLOAT, 0);
    case 17: return set(out, SR_TEX_FMT_RGBA16_FLOAT, 0);
    case 18: return set(out, SR_TEX_FMT_R32_FLOAT, 0);
    case 20: return set(out, SR_TEX_FMT_RGBA32_FLOAT, 0);
    case 24: return set(out, SR_TEX_FMT_BC6H_UFLOAT, 0);
    case 25: return set(out, SR_TEX_FMT_BC7, srgb);
    case 26: return set(out, SR_TEX_FMT_BC4, 0);
    case 27: return set(out, SR_TEX_FMT_BC5, 0);
    case 48: return set(out, SR_TEX_FMT_ASTC_4x4, srgb);
    case 49: return set(out, SR_TEX_FMT_ASTC_5x5, srgb);
    case 50: return set(out, SR_TEX_FMT_ASTC_6x6, srgb);
    case 51: return set(out, SR_TEX_FMT_ASTC_8x8, srgb);
    case 52: return set(out, SR_TEX_FMT_ASTC_10x10, srgb);
    case 53: return set(out, SR_TEX_FMT_ASTC_12x12, srgb);
    case 63: return set(out, SR_TEX_FMT_R8, 0);
    case 62: return set(out, SR_TEX_FMT_RG8, 0);   /* Unity RG16 = two 8-bit channels */
    default: return SR_TEX_E_FORMAT;
    }
}
