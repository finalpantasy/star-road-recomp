---
id: task-11
title: Texture path: block-linear deswizzle and ASTC decode
status: Verified
---
## Problem before solution
13,883 textures are block-linear swizzled, mostly ASTC 6x6. The GPU path needs linear data or a native ASTC upload.
## Outcome and scope
`runtime/gfx/texture/` (our own C or C++): deswizzle for every format and layout the game uses, ASTC decode to RGBA8 or BC7 fallback, plus an offline checker. Non-goal: HD texture packs.
## Dependencies and ownership
Depends on task-3. Owns `runtime/gfx/texture/`, `tests/texture/`.
Execution: subchat, Sonnet high.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Every Texture2D format and layout in the romfs is handled or listed | Survey run | Counts | PASS (technical): 13,883 bundle Texture2D + 99 in globalgamemanagers.assets, resources.assets, unity default resources + 215 Cubemap (6 faces) = 14,197 objects. 14,166 have data and all 14,166 match the layout model (offsets, total, stored size); 31 have no pixel data (0x0 or image-less objects such as runtime font atlases and placeholders), listed by id in the reports. 0 unhandled formats. Formats: ASTC 6x6 10,702, ASTC 5x5 1,191, ASTC 4x4 23, BC3 1,319, BC1 209, BC7 2, BC6H 89 (HDR cubemaps: layout handled, no decoder, passes to Vulkan natively), RGBA32 520, RGB24 13, A8 110, ARGB4444 19. Block heights 0..4 only, no 3D, no array textures, no mipsStripped. 578 empty-blob textures are tightly packed linear. Reports: A:\AI\Recomps\smrpg-work\textures\reports\survey.txt, survey_extra.txt |
| AC2 | ASTC decode bit-exact against ARM astcenc (test oracle, Apache-2.0, not linked) on all ASTC textures | Test | Report | PASS: astcenc 5.7.1 CLI (-dl and -ds), all 11,807 bundle ASTC textures (every mip) 532,731,690 blocks, 0 mismatching, 0 error-color blocks, 7,253 sRGB and 4,554 linear textures; plus 109 ASTC cubemaps (all 6 faces) 392,946 blocks, 0 mismatching; a re-run with the final source (every 40th texture, 290 textures, 11,599,043 blocks) also 0. Reports: A:\AI\Recomps\smrpg-work\textures\reports\astc_shard0..5.txt, astc_extra.txt, astc_final_sample.txt. Caveat: the oracle only exercises block types that occur in the game; LDR only (HDR endpoints give the error color, as astcenc LDR does), 2D only. |
| AC3 | Deswizzle round-trip test on all block sizes and mip chains | Test | Report | PASS: real data, all 14,166 textures with data (incl. cubemap faces) swizzle(deswizzle(x)) reproduces the stored bytes, padding is zero in every file, A:\AI\Recomps\smrpg-work\textures\reports\roundtrip.txt, roundtrip_extra.txt. Synthetic: tests/texture/test_texture.py (20 tests, 0 failed) covers all 82 formats, block sizes 4x4 to 12x12, block heights 0..5, full mip chains, arrays, cube, 3D, custom pitches, against an independent address model. |
## Human gates
None. Appearance and feel stay the owner's call (ART_DIRECTION).
## Recovery and return condition
Generated output and caches on A: only. Return once at done or blocked.
## Approach and commands
docs/RECOMPILER.md is the design. D-008: our own code; Snap (A:\AI\Recomps
ew-pokemon-snap) and Ryubing (A:\AI\Recomps
ew-pokemon-snap	oolsefyubing-ryujinx) may be read for lessons and run as test oracles only, never copied.
## Log and handoff
Record failed hypotheses and measured numbers.

### Evidence (worker, 2026-11-24; nothing committed)
- Code: runtime/gfx/texture/ (texture.h API, tex_format.c, tex_layout.c, tex_astc.c, tex_bc.c, tools/ texture_check + manifest exporters, CMakeLists.txt), tests/texture/ (texture_unit.c, test_texture.py).
- Outputs (A: only): textures\manifest.tsv, manifest_extra.tsv, reports\*, dump\ PNGs. Rebuild: python tests/texture/test_texture.py; checker via runtime/gfx/texture/tools (see hand-over note).
- Layout facts: m_PlatformBlob word2 = log2 block height, words 4.. = per-mip offsets, word 21 = total; empty blob = tightly packed linear chain; Cubemap faces are linear, face after face, m_CompleteImageSize = one face; RGB24 with a blob is stored as 4 bytes per texel.
- ASTC fixes that the oracle forced (kept in tex_astc.c): rounding (endpoint * 257, lerp +32 >> 6, result >> 8), sRGB endpoint (e<<8)|0x80, quint/trit color unquantization constants, quint weight constant, partition hash shifts.
- BC7 transcode (ASTC to BC7 mode 6, 121 sampled textures): mean PSNR 48.08 dB, min 21.35 dB, 4.4 Mpix/s single thread. Lossy second pass; native ASTC stays the faithful mode.
- Not covered: BC6H decode (not needed: desktop GPUs sample it); non-empty-blob multi-image layer stride (no such texture in the dump); 3D/array layout beyond the model; HDR ASTC; Sprite/RenderTexture objects.
