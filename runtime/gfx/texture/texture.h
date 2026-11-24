/* Star Road Recomp: texture path (task-11). Our own code (D-008).
 *
 * WHAT THIS IS
 *   Turns guest texture memory (Maxwell block-linear "GOB" layout, as the Switch GPU and NVN see it) into
 *   data a PC GPU can use. No game data lives here; callers hand in guest bytes.
 *
 * WHO CALLS IT
 *   task-12 (NVN on Vulkan) at nvnTextureBuilder / nvnTextureView / nvnCommandBufferCopyBufferToTexture time,
 *   and the offline checker (texture_check) over the owner's Unity bundles.
 *
 * THE FIVE CALLS YOU NEED (everything else is detail)
 *   1. sr_tex_format_from_nvn(nvn_format, &fmt)      NVN Format enum value -> sr_tex_format (block size, bytes).
 *      sr_tex_format_from_unity(unity_tf, srgb, &fmt) Unity TextureFormat id -> sr_tex_format (offline tools).
 *   2. sr_tex_plan_build(&desc, &plan)                Layout of the swizzled surface: per mip offset, size,
 *                                                     block height, plus layer stride and total size. Needs only
 *                                                     size/format/levels/layers (+ optional block height from the
 *                                                     NVN texture's tile mode).
 *      sr_tex_plan_from_unity_blob(...)               Same, but takes the block height and mip offsets from
 *                                                     Unity's 92-byte m_PlatformBlob and verifies the model
 *                                                     against the blob's own offsets (strict).
 *   3. sr_tex_prepare_upload(&plan, guest_ptr, guest_size, target, caps, &up)
 *                                                     One call: deswizzle every (layer, mip), decode or transcode
 *                                                     if the GPU cannot take the format, and return a staging
 *                                                     buffer plus a region list shaped like vkCmdCopyBufferToImage
 *                                                     (buffer offset, mip, layer, extent, bytes per row in blocks).
 *      sr_tex_upload_free(&up)                       Frees staging memory.
 *   4. Low level, if you want to drive Vulkan yourself:
 *      sr_tex_deswizzle_level / sr_tex_swizzle_level  One (layer, mip) between swizzled and tightly packed
 *                                                     linear blocks. Round-trips bit-exactly on valid bytes.
 *   5. sr_tex_decode_astc_ldr(...) / sr_tex_encode_bc7(...)  Block codecs used by sr_tex_prepare_upload; exposed
 *                                                     for tests and for callers that want only the codec.
 *
 * TARGET MODES (sr_tex_target)
 *   NATIVE  Keep the guest format. BC1..BC7 and uncompressed formats go to Vulkan as is. ASTC also stays ASTC
 *           (use when caps.astc is true).
 *   RGBA8   ASTC is decoded to RGBA8 (sRGB textures yield sRGB-encoded bytes; sample with an *_SRGB format).
 *   BC7     ASTC is decoded then re-encoded to BC7 (4x4 blocks, lossy second pass; use when caps.bc7 and not
 *           caps.astc). Non-ASTC formats behave as in NATIVE.
 *   AUTO    NATIVE if the GPU has the format, else BC7 if caps.bc7, else RGBA8.
 *
 * THREADING: all functions are reentrant and take no locks; do not touch the same upload from two threads.
 * MEMORY: the library allocates only through the optional hooks in sr_tex_set_allocator (default: malloc).
 * ERRORS: every function returns SR_TEX_OK (0) or a negative sr_tex_status; nothing aborts.
 *
 * LAYOUT MODEL (Tegra X1 / Maxwell block-linear, verified against all 13,883 Unity textures in the dump)
 *   GOB = 64 bytes x 8 rows (512 B), bits interleaved as in sr_tex_gob_offset(). A block is bh GOBs stacked in Y
 *   (and bd GOBs in Z for 3D). Blocks run in X across the row, then Y, then Z. A mip's bh shrinks by halves until
 *   8*(bh/2) >= block rows. Mips are packed one after another with no extra alignment in Unity blobs (offsets
 *   come from the blob); layers (arrays, cube faces) use a stride aligned to one block of mip 0.
 */
#ifndef SR_TEXTURE_H
#define SR_TEXTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_TEX_MAX_LEVELS 16

typedef enum sr_tex_status {
    SR_TEX_OK = 0,
    SR_TEX_E_ARG = -1,        /* bad pointer, zero size, unsupported combination */
    SR_TEX_E_FORMAT = -2,     /* format unknown or not handled by this call */
    SR_TEX_E_RANGE = -3,      /* guest buffer too small for the plan, or size overflow */
    SR_TEX_E_NOMEM = -4,
    SR_TEX_E_BLOB = -5,       /* Unity platform blob malformed */
    SR_TEX_E_MISMATCH = -6,   /* layout model disagrees with the blob (offsets or total size) */
    SR_TEX_E_CORRUPT = -7     /* undecodable data; output filled with the error color where possible */
} sr_tex_status;

/* Codec family of a format. */
typedef enum sr_tex_codec {
    SR_TEX_CODEC_RAW = 0,     /* one texel per block, plain bytes */
    SR_TEX_CODEC_BC1, SR_TEX_CODEC_BC2, SR_TEX_CODEC_BC3, SR_TEX_CODEC_BC4, SR_TEX_CODEC_BC5,
    SR_TEX_CODEC_BC6H, SR_TEX_CODEC_BC7,
    SR_TEX_CODEC_ASTC
} sr_tex_codec;

/* Format ids. Raw formats list memory byte order for little-endian hosts where it matters. */
typedef enum sr_tex_fmt {
    SR_TEX_FMT_NONE = 0,
    /* raw, 1 byte */
    SR_TEX_FMT_R8, SR_TEX_FMT_R8_SNORM, SR_TEX_FMT_R8_UINT, SR_TEX_FMT_R8_SINT, SR_TEX_FMT_A8,
    /* raw, 2 bytes */
    SR_TEX_FMT_R16_FLOAT, SR_TEX_FMT_R16, SR_TEX_FMT_R16_SNORM, SR_TEX_FMT_R16_UINT, SR_TEX_FMT_R16_SINT,
    SR_TEX_FMT_RG8, SR_TEX_FMT_RG8_SNORM, SR_TEX_FMT_RG8_UINT, SR_TEX_FMT_RG8_SINT,
    SR_TEX_FMT_RGBA4, SR_TEX_FMT_ARGB4444, SR_TEX_FMT_RGB5A1, SR_TEX_FMT_RGB565, SR_TEX_FMT_BGR5A1,
    /* raw, 3 bytes */
    SR_TEX_FMT_RGB8,
    /* raw, 4 bytes */
    SR_TEX_FMT_R32_FLOAT, SR_TEX_FMT_R32_UINT, SR_TEX_FMT_R32_SINT,
    SR_TEX_FMT_RG16_FLOAT, SR_TEX_FMT_RG16, SR_TEX_FMT_RG16_SNORM, SR_TEX_FMT_RG16_UINT, SR_TEX_FMT_RG16_SINT,
    SR_TEX_FMT_RGBA8, SR_TEX_FMT_RGBA8_SNORM, SR_TEX_FMT_RGBA8_UINT, SR_TEX_FMT_RGBA8_SINT,
    SR_TEX_FMT_BGRA8, SR_TEX_FMT_ARGB8,
    SR_TEX_FMT_RGB10A2, SR_TEX_FMT_RGB10A2_UINT, SR_TEX_FMT_R11G11B10_FLOAT, SR_TEX_FMT_RGB9E5,
    /* raw, 6 bytes and up */
    SR_TEX_FMT_RGB16_FLOAT,
    SR_TEX_FMT_RG32_FLOAT, SR_TEX_FMT_RG32_UINT, SR_TEX_FMT_RG32_SINT,
    SR_TEX_FMT_RGBA16_FLOAT, SR_TEX_FMT_RGBA16, SR_TEX_FMT_RGBA16_SNORM, SR_TEX_FMT_RGBA16_UINT, SR_TEX_FMT_RGBA16_SINT,
    SR_TEX_FMT_RGB32_FLOAT,
    SR_TEX_FMT_RGBA32_FLOAT, SR_TEX_FMT_RGBA32_UINT, SR_TEX_FMT_RGBA32_SINT,
    /* depth and stencil */
    SR_TEX_FMT_STENCIL8, SR_TEX_FMT_DEPTH16, SR_TEX_FMT_DEPTH24, SR_TEX_FMT_DEPTH32_FLOAT,
    SR_TEX_FMT_DEPTH24_STENCIL8, SR_TEX_FMT_DEPTH32_FLOAT_STENCIL8,
    /* block compressed, 4x4 */
    SR_TEX_FMT_BC1_RGB, SR_TEX_FMT_BC1_RGBA, SR_TEX_FMT_BC2, SR_TEX_FMT_BC3,
    SR_TEX_FMT_BC4, SR_TEX_FMT_BC4_SNORM, SR_TEX_FMT_BC5, SR_TEX_FMT_BC5_SNORM,
    SR_TEX_FMT_BC6H_UFLOAT, SR_TEX_FMT_BC6H_SFLOAT, SR_TEX_FMT_BC7,
    /* ASTC LDR, 16 bytes per block */
    SR_TEX_FMT_ASTC_4x4, SR_TEX_FMT_ASTC_5x4, SR_TEX_FMT_ASTC_5x5, SR_TEX_FMT_ASTC_6x5, SR_TEX_FMT_ASTC_6x6,
    SR_TEX_FMT_ASTC_8x5, SR_TEX_FMT_ASTC_8x6, SR_TEX_FMT_ASTC_8x8, SR_TEX_FMT_ASTC_10x5, SR_TEX_FMT_ASTC_10x6,
    SR_TEX_FMT_ASTC_10x8, SR_TEX_FMT_ASTC_10x10, SR_TEX_FMT_ASTC_12x10, SR_TEX_FMT_ASTC_12x12,
    SR_TEX_FMT__COUNT
} sr_tex_fmt;

/* A format plus its color space. srgb applies to RGBA8/BGRA8/BC1-3/BC7/ASTC (ignored elsewhere). */
typedef struct sr_tex_format {
    sr_tex_fmt fmt;
    uint8_t srgb;
} sr_tex_format;

typedef struct sr_tex_format_info {
    const char* name;
    sr_tex_codec codec;
    uint8_t block_w, block_h;    /* texels per block */
    uint8_t bytes_per_block;     /* raw: bytes per texel */
    uint8_t channels;            /* 1..4, 0 for depth/stencil packs */
    uint8_t is_depth_stencil;
} sr_tex_format_info;

/* Table lookup. Returns NULL for NONE / out of range. */
const sr_tex_format_info* sr_tex_format_info_get(sr_tex_fmt fmt);

/* NVN Format enum values (nvn::Format) -> format. Unknown values return SR_TEX_E_FORMAT. Values follow
 * NVN's published ordering: 0x01.. R8..RG32, 0x25.. RGBA8.., 0x41..0x50 BC, 0x79..0x94 ASTC (linear then sRGB). */
int sr_tex_format_from_nvn(uint32_t nvn_format, sr_tex_format* out);

/* Unity TextureFormat id (e.g. 50 = ASTC_6x6, 12 = DXT5, 4 = RGBA32). */
int sr_tex_format_from_unity(int unity_texture_format, int srgb, sr_tex_format* out);

/* ---- layout --------------------------------------------------------------------------------------------- */

typedef enum sr_tex_dim {
    SR_TEX_DIM_2D = 0,   /* also 1D (height 1), rectangle */
    SR_TEX_DIM_3D = 1,
    SR_TEX_DIM_CUBE = 2  /* layers counts faces: 6 per cube */
} sr_tex_dim;

#define SR_TEX_BH_AUTO (-1)

typedef struct sr_tex_surface_desc {
    sr_tex_format format;
    uint32_t width, height;
    uint32_t depth;               /* 3D slices; 1 for 2D */
    uint32_t layers;              /* array layers (cube: faces); 0 treated as 1 */
    uint32_t levels;              /* mip count, 1..SR_TEX_MAX_LEVELS */
    sr_tex_dim dim;
    /* Block height (in GOBs, as log2: 0..4) of mip 0. SR_TEX_BH_AUTO picks NVN's default rule from the height;
     * pass the texture's tile mode when you have it. Block depth (3D only) likewise. */
    int32_t block_height_log2;
    int32_t block_depth_log2;
} sr_tex_surface_desc;

typedef struct sr_tex_level_plan {
    uint32_t width, height, depth;      /* texels */
    uint32_t blocks_w, blocks_h;        /* codec blocks */
    uint8_t bh_log2, bd_log2;           /* block height / depth in GOBs for this mip */
    uint64_t offset;                    /* byte offset of this mip inside one layer, swizzled data */
    uint64_t size;                      /* swizzled bytes of this mip for one layer (padded) */
    uint64_t linear_offset;             /* byte offset inside one layer of the tightly packed linear form */
    uint64_t linear_size;               /* blocks_w * blocks_h * depth * bytes_per_block */
} sr_tex_level_plan;

typedef struct sr_tex_plan {
    sr_tex_surface_desc desc;           /* normalized copy (layers >= 1, depth >= 1) */
    sr_tex_level_plan level[SR_TEX_MAX_LEVELS];
    uint64_t layer_stride;              /* swizzled bytes between layers */
    uint64_t total_size;                /* swizzled bytes for the whole surface */
    uint64_t linear_layer_stride;       /* linear bytes between layers */
    uint64_t linear_total_size;
    /* 1 when the stored bytes are already the tightly packed linear mip chain (Unity textures with an empty
     * m_PlatformBlob: tiny UI and editor textures). Then offset/size/total_size describe that packed form and
     * (de)swizzle is a plain row copy. 0 for block-linear data. */
    uint8_t stored_linear;
} sr_tex_plan;

/* Build the layout model (no blob). */
int sr_tex_plan_build(const sr_tex_surface_desc* desc, sr_tex_plan* plan);

/* Unity Switch Texture2D: format id, srgb, size, mip count, the 92-byte m_PlatformBlob and the stored image
 * size (m_CompleteImageSize). Block height and mip offsets are read from the blob and the model is checked
 * against every offset and the total. Returns SR_TEX_E_MISMATCH with plan filled from the model if they differ. */
int sr_tex_plan_from_unity_blob(int unity_format, int srgb, uint32_t width, uint32_t height, uint32_t mips,
                                const uint8_t* blob, size_t blob_size, uint64_t complete_image_size,
                                sr_tex_plan* plan);

/* Same for Unity Cubemap / array objects: image_count images (6 for a cubemap). m_CompleteImageSize and the blob's
 * total describe ONE image. Every Cubemap in the owner's dump has an empty blob (tightly packed linear faces, face
 * after face, each face holding its whole mip chain); a non-empty blob with several images follows the aligned layer
 * stride of sr_tex_plan_build, which no data in the dump exercises. */
int sr_tex_plan_from_unity_blob_ex(int unity_format, int srgb, uint32_t width, uint32_t height, uint32_t mips, uint32_t image_count,
                                   const uint8_t* blob, size_t blob_size, uint64_t complete_image_size, sr_tex_plan* plan);

/* Offset of byte (x, y) inside one GOB (x in 0..63, y in 0..7): the Maxwell bit interleave. */
static inline uint32_t sr_tex_gob_offset(uint32_t x, uint32_t y) {
    return ((x >> 5) << 8) | (((y >> 1) & 3) << 6) | (((x >> 4) & 1) << 5) | ((y & 1) << 4) | (x & 15);
}

/* Default block height (log2 GOBs) NVN uses for a mip-0 height in blocks. */
uint32_t sr_tex_default_bh_log2(uint32_t height_in_blocks);

/* Swizzled <-> linear for one (layer, level). `swz` points at the start of the whole swizzled surface.
 * `lin` is a tightly packed buffer of level.linear_size bytes (rows blocks_w * bytes_per_block, slices after
 * rows) unless pitches are given (0 = tight). Padding bytes in the swizzled data are written as zero on swizzle
 * and ignored on deswizzle. Bounds are checked against swz_size. */
int sr_tex_deswizzle_level(const sr_tex_plan* plan, uint32_t layer, uint32_t level,
                           const uint8_t* swz, size_t swz_size, uint8_t* lin, uint64_t row_pitch, uint64_t slice_pitch);
int sr_tex_swizzle_level(const sr_tex_plan* plan, uint32_t layer, uint32_t level,
                         const uint8_t* lin, uint64_t row_pitch, uint64_t slice_pitch, uint8_t* swz, size_t swz_size);

/* ---- upload --------------------------------------------------------------------------------------------- */

typedef enum sr_tex_target { SR_TEX_TARGET_AUTO = 0, SR_TEX_TARGET_NATIVE, SR_TEX_TARGET_RGBA8, SR_TEX_TARGET_BC7 } sr_tex_target;

typedef struct sr_tex_gpu_caps {
    uint8_t astc;     /* GPU samples ASTC LDR (mobile, Intel iGPU, some others) */
    uint8_t bc7;      /* GPU samples BC1..BC7 (all desktop GPUs) */
} sr_tex_gpu_caps;

typedef struct sr_tex_region {
    uint64_t buffer_offset;       /* in staging */
    uint64_t byte_size;
    uint32_t level, layer;
    uint32_t width, height, depth;          /* texel extent of the mip */
    uint32_t buffer_row_length;             /* texels, rounded up to the block size (Vulkan bufferRowLength) */
    uint32_t buffer_image_height;           /* texels, rounded up to the block size */
} sr_tex_region;

typedef struct sr_tex_upload {
    sr_tex_format format;         /* what the staging data is: pass this to the GPU image */
    sr_tex_region* regions;       /* levels * layers entries, layer-major */
    uint32_t region_count;
    uint8_t* data;                /* staging bytes */
    uint64_t data_size;
    uint32_t error_blocks;        /* ASTC blocks that were invalid and got the error color */
} sr_tex_upload;

int sr_tex_prepare_upload(const sr_tex_plan* plan, const uint8_t* guest, size_t guest_size,
                          sr_tex_target target, sr_tex_gpu_caps caps, sr_tex_upload* out);
void sr_tex_upload_free(sr_tex_upload* up);

/* Optional allocator hooks (default malloc/free). */
void sr_tex_set_allocator(void* (*alloc)(size_t), void (*release)(void*));

/* ---- codecs --------------------------------------------------------------------------------------------- */

/* Decode ASTC LDR blocks (2D, 16 bytes each, row-major blocks) into RGBA8, tightly packed, width*4 bytes per row.
 * srgb selects the sRGB profile (decode returns sRGB-encoded bytes; no linearization). Invalid blocks and HDR
 * endpoint blocks get magenta (255,0,255,255) like astcenc's LDR profile; their count goes to *error_blocks. */
int sr_tex_decode_astc_ldr(const uint8_t* blocks, uint32_t block_w, uint32_t block_h, uint32_t width, uint32_t height,
                           int srgb, uint8_t* rgba8, uint32_t* error_blocks);

/* Decode one ASTC block into block_w * block_h RGBA texels as 16-bit UNORM (before the final 8-bit conversion),
 * (sRGB blocks use the sRGB endpoint expansion; the 8-bit result is value >> 8 in both profiles).
 * Returns 0 valid, 1 error (error color written). */
int sr_tex_decode_astc_block(const uint8_t block[16], uint32_t block_w, uint32_t block_h, int srgb, uint16_t* rgba16);

/* Encode RGBA8 into BC7 mode 6 (single subset, 7-bit RGBA endpoints plus p-bits, 4-bit indices); lossy.
 * Output (w+3)/4 * (h+3)/4 blocks of 16 bytes. quality 0 = one PCA pass, 1 = adds least-squares refinement. */
int sr_tex_encode_bc7(const uint8_t* rgba8, uint32_t width, uint32_t height, int quality, uint8_t* blocks);

/* Reference decoders for tests and CPU previews. BC7 decodes mode 6 only (what sr_tex_encode_bc7 emits); any other
 * mode returns SR_TEX_E_FORMAT after filling gray. */
int sr_tex_decode_bc7(const uint8_t* blocks, uint32_t width, uint32_t height, uint8_t* rgba8);
int sr_tex_decode_bc1(const uint8_t* blocks, uint32_t width, uint32_t height, uint8_t* rgba8);
int sr_tex_decode_bc3(const uint8_t* blocks, uint32_t width, uint32_t height, uint8_t* rgba8);

/* Convert any raw format from the table to RGBA8 for previews (8-bit unorm formats and 4/5/6-bit packs only).
 * Returns SR_TEX_E_FORMAT for other formats. */
int sr_tex_raw_to_rgba8(sr_tex_fmt fmt, const uint8_t* src, uint32_t width, uint32_t height, uint8_t* rgba8);

#ifdef __cplusplus
}
#endif
#endif /* SR_TEXTURE_H */
