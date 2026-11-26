# Shader cache interface (task-10 output, task-12 input)

Status: v1, stable. Changes that break readers bump the directory version (`v1` to `v2`) and are
recorded at the end of this file. Producer: `tools/shaderxlate/` (task-10). Consumer: `runtime/nvn/`
(task-12). Nothing here enters Git: the cache is rebuilt from the owner's own dump.

## 1. What a cache entry is

One entry per unique NVN stage program (one glslc data section). Unity on NVN stores each shader
variant as one linked NVN program (all stages in the vertex sub-program); the same stage program is
shared by many variants, so the cache is keyed by stage program, not by Unity variant.

**Key**: lowercase hex SHA-256 of the stage's data section bytes, exactly `data_size` bytes, where
`data_size` is the u32 at control section offset `+0x704`. The runtime computes it at
`nvnProgramSetShaders`: for each `NVNshaderData`, read `data_size` from the control section (CPU
pointer), hash `data_size` bytes of guest GPU memory at the data address. No translator version or
salt goes into the key; the directory version covers translator changes.

Control section fields the runtime may read (all u32, offsets from the start of the control
section, magic `0x98761234` at +0): `+0x6F8` program size (SPH plus code), `+0x6FC` constant size,
`+0x700` constant offset inside data, `+0x704` data size, `+0x708` SPH offset inside data (0x30).

## 2. Files

Root: `A:\AI\Recomps\smrpg-work\shaders\cache\v1\` (the batch driver takes `--out`; a user build
uses its data folder with the same layout).

| Path | Content |
|---|---|
| `index.jsonl` | One JSON object per line, one line per stage program (section 3). Load this once at start. |
| `spv/<key[0:2]>/<key>.spv` | SPIR-V 1.3 module, entry point `main`, little-endian words. Present only when `status` is `ok`. |
| `meta/<key[0:2]>/<key>.json` | Stage manifest (section 4), the same object as the index line plus resources. |
| `variants.jsonl` | Unity variant to stage keys (shader name, pass, keywords, tier); debugging and A/B only. |
| `report.json` | Batch counts, failures by reason, spirv-val result. |

Writers publish an entry by writing the `.spv`, then the `.json` by rename; a reader treats an entry
as present only when the index line says `ok` and both files load.

## 3. Index line

```json
{"key": "<64 hex>", "stage": "vs|fs|gs|tcs|tes|cs", "status": "ok|failed",
 "reason": "<text when failed>", "spv": "spv/ab/<key>.spv", "meta": "meta/ab/<key>.json",
 "insns": 123, "translator": "shaderxlate 1"}
```

`failed` entries name the reason (for example an unsupported opcode with its address). For a
Programs the game binds that are not in its Unity shader bundles (library shaders, dumped by the
runtime with `NVN_DUMP_MISSING=<shaders>/missing`, same `<key>.data` plus `<key>.ctrl` layout and
the same key) are translated by every full `batch.py` run and their lines add `"source":
"nvn_dump"`; their stage comes from control section word `+0x714` (NVN stage: 0 vertex, 1 fragment,
2 geometry, 3 tess control, 4 tess evaluation, 5 compute).

failed key the runtime uses the fallback chain in RECOMPILER.md section 5: a hand-written
replacement at `replace/<key>.spv` plus `replace/<key>.json` (same manifest format) if present,
otherwise it skips the draw and logs the key once.

## 4. Stage manifest

```json
{
 "key": "...", "stage": "fs", "spirv_version": "1.3",
 "sets": {"set": 1},
 "ubos":     [{"binding": 1, "cbuf": 3, "nvn_index": 0, "size": 256}],
 "textures": [{"binding": 2, "dim": "2d|3d|cube|2d_array|cube_array|1d|buffer|2dms",
               "shadow": false, "array_len": 1,
               "handle": {"cbuf": 2, "offset": 32},
               "sampler_handle": null,
               "nvn": {"kind": "combined", "index": 0}}],
 "images":   [{"binding": 5, "dim": "2d", "format": "r32ui|...|unknown", "handle": {"cbuf": 2, "offset": 64},
               "nvn": {"kind": "image", "index": 0}}],
 "ssbos":    [{"binding": 6, "desc": {"cbuf": 0, "offset": 272}, "nvn_index": 0, "write": false,
               "base_align": 256}],
 "inputs":   [{"location": 0, "attr": 0, "components": "xyzw", "interp": "smooth|flat|noperspective"}],
 "outputs":  [{"location": 0, "components": "xyzw"}],
 "builtins_in": ["frag_coord", "front_facing", "instance_id", "vertex_id"],
 "builtins_out": ["position", "point_size", "clip_distance[2]", "layer", "viewport_index", "frag_depth"],
 "frag": {"rt_mask": 1, "writes_depth": false, "early_fragment_tests": false, "discards": true},
 "local_size": [64, 1, 1], "shared_bytes": 0, "local_bytes": 0,
 "requires": {"subgroup_size_32": false, "fragment_shader_interlock": false, "geometry_passthrough": false},
 "constants_inlined": true
}
```

Rules the consumer can rely on:

- **Descriptor sets**: one set per stage. `vs` = set 0, `fs` = set 1, `gs` = set 2, `tcs` = set 3,
  `tes` = set 4; `cs` = set 0. `sets.set` repeats it. Binding 0 of every set is reserved for a
  translator support buffer (not emitted in v1; declare nothing there). Other bindings are compact
  per stage program in the order UBOs, textures, images, SSBOs, and are listed in the manifest. The
  consumer builds one `VkDescriptorSetLayout` per stage program (cache it by key) and one pipeline
  layout per stage combination. Descriptor types: UBO = `UNIFORM_BUFFER`, texture =
  `COMBINED_IMAGE_SAMPLER` (`buffer` dim = `UNIFORM_TEXEL_BUFFER`), image = `STORAGE_IMAGE`, SSBO =
  `STORAGE_BUFFER`. All stage flags are the entry's own stage.
- **Constant buffers**: `cbuf` is the Maxwell slot `c[n]`. NVN binds `nvnCommandBufferBindUniformBuffer(stage, i)`
  to `c[i + 3]`, so `nvn_index = cbuf - 3`. `c[1]` (the program's own constants) is inlined into the
  SPIR-V (`constants_inlined`), never bound. Reads of `c[0]` and `c[2]` that are not resolved as
  handles or storage descriptors are listed as UBOs with `nvn_index` < 0; the runtime binds a
  host-built buffer with the driver words it models (NVN c[0] driver constants, c[2] handle table).
  `size` is the highest byte the shader can read, rounded up to 16 (indexed reads give the full
  64 KiB).
- **Textures**: Maxwell texture instructions name a 32-bit handle word in `c[2]` (bound) or build
  the handle in a register (bindless `.B`). The translator traces bindless handles back to constant
  buffer loads. `handle` is the location of the texture handle word, `sampler_handle` the location
  of a separate sampler word OR-ed into it (NVN separate textures at `c[2][0x168 + 8r]`, separate
  samplers at `c[2][0x568 + 8s]`), or null for a combined handle. `nvn.kind` is `combined`
  (`index = (offset - 0x20) / 8`), `separate` (`index` r and `sampler_index` s) or `raw` (location
  outside the known NVN ranges; resolve from the words at `handle`). The runtime reads the bound
  handle words for that draw, decodes texture id and sampler id from its own handle tables, and
  writes one combined descriptor. An NVN combined handle is `texture_id | sampler_id << 20`.
  `array_len` > 1 means an indexed handle array (`handle.offset` is element 0, stride 8).
  The trace follows register moves and ORs across blocks and loops, spills to local memory at
  constant addresses (`STL` then `LDL` of the same word), and predicated writes: a texture
  instruction under predicate P sees a handle written under the opposite polarity of P only as the
  value from before that write. Multisample fetches (`TLD.MS`, 2D only) use a multisampled image
  (`dim` `2dms`, entry adds `"multisample": true`) and pass the sample index as the `Sample` operand.
  Size queries (`TXQ.B`, and bound `TXQ` whose handle is word `TidB` of `c[2]`, query 1 only:
  width, height, depth or layers at the given level, and the level count) use the binding of another
  access to the same handle (a storage image first, then a sampled texture); a query with no other
  access gets a 2D texture binding, 3D when it asks for depth. A component the image type does not
  have reads 1.
- **Images**: `SUST.B` (formatted store, full RGBA mask, ignore-out-of-range clamp) writes a storage
  image declared with format `Unknown` (`shaderStorageImageWriteWithoutFormat`) and float texels.
  `handle` is the `c[2]` word of the bindless handle; `nvn.kind` `image` with `index = (offset -
  0x120) / 8` (NVN image slots follow the 32 combined texture handles at `0x20 + 8k`), or `raw` with
  `index` -1 outside that range. The runtime binds the image the program's `nvnCommandBufferBindImage`
  slot names as `STORAGE_IMAGE`.
- **Storage buffers**: global memory accesses whose address comes from an NVN storage descriptor
  (`c[0][0x110 + 16i]`: u64 address, u32 size) become SSBO `nvn_index` i, declared as a
  `uint[]` runtime array (`NonWritable` when `write` is false). The shader reads the descriptor's
  low address word from `c[0]` and indexes the SSBO with `address - (base & ~(base_align - 1))`,
  so the runtime binds SSBO i at byte `base & ~255` of the guest buffer (256 is the largest
  `minStorageBufferOffsetAlignment` Vulkan allows) with a range that reaches `base + size`. `c[0]`
  is then also listed as a UBO and must hold the real descriptor words. The LDG address is traced
  to the descriptor inside its block; when the base was added in another block and the program
  reads exactly one descriptor, that one is used. Global accesses that cannot be traced fail
  translation (listed with a reason) rather than guess. Out-of-range reads are not clamped: the
  runtime enables `robustBufferAccess` (they read 0 under `robustBufferAccess2`).
- **Vertex inputs**: generic attribute `attr` (Maxwell `a[0x80 + 16*attr]`) is `location = attr`,
  declared `vec4` of float. Only vertex-stage inputs carry `attr`; inputs of other stages are
  varyings with `location` only. Integer vertex formats are not expected from Unity on this game; the
  runtime must check the bound vertex formats and log any integer format as a mismatch.
- **Varyings**: generic output `i` (`a[0x80 + 16*i]`) is `location = i` in every stage, so stages
  link by location. Maxwell supplies zero for a missing fragment input. At pipeline creation the
  Vulkan runtime checks the immediately preceding graphics stage's SPIR-V interface and creates a
  fragment variant with zero-initialized private values for absent locations. It caches the
  variant by stage pair, so the same fragment shader can still receive a varying from a producer
  that writes it. The on-disk shader cache is read-only.
- **Geometry stage**: a geometry program reads per-vertex inputs through the `ISBERD` vertex handle;
  the translator models one input primitive per invocation (invocation info low byte 0, as on the
  hardware for a non-instanced draw), so the handle is the vertex index. Inputs are arrays sized to
  the input primitive (`in<loc>[3]`, `gl_in[3].Position`). The manifest's `geometry` object gives
  `input` (`triangles` in v1; SR invocation info reports 3 vertices), `input_vertices`, `output`
  (`points`, `line_strip` or `triangle_strip`, from the SPH), `max_vertices` and `invocations`. The
  consumer must draw such a pipeline with a triangle topology; any other input topology needs a new
  variant and is logged. `OUT.EMIT`/`OUT.CUT` become `OpEmitVertex`/`OpEndPrimitive`; `Layer` and
  `ViewportIndex` outputs need no extension in this stage.
- **Fragment outputs**: render target `i` is `location = i`, `vec4` float. `rt_mask` has bit `i` set
  for each written target. Depth writes use `FragDepth`.
- **Coordinates**: positions are passed through unchanged. NVN depth mode near = 0 matches Vulkan;
  NVN keeps +y up, so the consumer uses a negative-height viewport. `FragCoord` is
  framebuffer-relative with an upper-left origin, as in NVN's upper-left window origin mode.
  Position is declared `Invariant`.
- **Subgroups**: Maxwell warps are 32 lanes. A stage with `requires.subgroup_size_32` uses subgroup
  operations (shuffle, vote, ballot) and must run with a required subgroup size of 32
  (`VK_EXT_subgroup_size_control`); on a device without it the runtime logs the key once.
- **Local memory**: `local_bytes` is the per-invocation local array size the translator sized from
  the program (largest `LDL`/`STL` offset plus width); nothing to bind.
- **Compute**: `local_size` is the workgroup size in the program's control section (words
  `+0x748`, `+0x74c`, `+0x750`). The module declares `LocalSize` with those values and a
  `WorkgroupSize` built-in made of specialization constants ids 0, 1, 2 (same defaults), which the
  consumer may override. `S2R` `SR_TID` and `SR_CTAID` read `LocalInvocationId` and `WorkgroupId`.
  `shared_bytes` is the size the program can address.
- **Control flow**: `cf_mode` is `structured` (Maxwell branches rebuilt as SPIR-V selection and
  loop constructs) or `dispatcher` (a block-index loop with a switch, used when the CFG cannot be
  structured; `cf_fallback` gives the reason, null otherwise). Both are valid SPIR-V; dispatcher
  modules are slower and are counted in `report.json` so they can be tracked down.
- **Required device features** (the consumer checks them once at device creation):
  Vulkan 1.3 or SPIR-V 1.3 support, `shaderDemoteToHelperInvocation` (fragment `KIL` is
  `OpDemoteToHelperInvocation`, so derivatives stay valid after a discard), subgroup basic and
  quad operations in fragment and compute stages (`SHFL` and derivative tricks), `shaderClipDistance`,
  `imageCubeArray`, `sampled1D`, `shaderImageGatherExtended`, and
  `shaderOutputViewportIndex`/`shaderOutputLayer` when a vertex stage writes those builtins,
  and `robustBufferAccess` when a module has SSBOs.
- **Rounding faithfulness**: half-precision ops (`HADD2`, `HMUL2`, `HFMA2`) round each result to
  fp16 when it is written to a register as a half pair, exactly as the hardware stores it.
  `FFMA.FMZ` stays one fused multiply-add with the Maxwell zero rule. These are deliberate: the
  Ryubing translator used as test oracle skips the fp16 rounding when a packed half is unpacked
  again, which is not what the hardware does.
- **Oracle-compat switches**: the environment variable `SXL_COMPAT` (comma list) changes our output
  only to mirror known oracle behavior during differential tests: `ipa_mul` (literal
  `IPA.MUL` multiply), `half_fold` (skip fp16 rounding between half ops), `no_fmz` (ignore the
  `FMUL`/`FFMA` `.FMZ` zero rule) and `fminmax` (`FMin`/`FMax` instead of `NMin`/`NMax`, so a NaN
  operand gives an undefined result instead of the other operand) and `gen_null` (generic
  outputs start at 0 instead of (0, 0, 0, 1); Ryubing initializes only the components the next
  stage reads, so with no next stage in the harness the rest read 0). The shipped cache is
  always built with `SXL_COMPAT` unset (`batch.py` refuses to run while it is set).
- **Carry flag**: `IADD.CC`/`.X` carries are internal to a module and add no interface.

## 5. Phase A cache (`reuse-v1`, D-009)

Until our own translator matches it, the runtime loads a cache made by the Ryubing translator (run
through the owner's Snap ShaderHarness, listed in docs/REUSE_LEDGER.md). Root
`A:\AI\Recomps\smrpg-work\shaders\cache\reuse-v1\`, same files and keys as section 2, built by
`reuse/shaders/ryu_cache.py` (gitignored). Index lines carry `"translator": "ryubing <commit> via
snap ShaderHarness <commit> (reused, D-009)"`. Everything in sections 3 and 4 holds, with these
differences a reader must handle (all additive; a reader written for both caches checks the fields):

- **Program constants are bound, not inlined**: `constants_inlined` is false and `ubos` has an entry
  with `cbuf: 1`, `nvn_index: -1`, `source: "program_constants"` and `file:
  "const/<key[0:2]>/<key>.bin"`, the program's constant section (the same bytes as guest data at
  control `+0x700`, `+0x6FC` bytes long). Bind it as a 64 KiB zero-padded uniform buffer.
- **UBO `source`**: `nvn` (`cbuf` >= 3, bind `nvn_index`), `driver_c0` (c[0]), `handles_c2` (c[2]),
  `program_constants` (c[1]). `size` is the declared block size (64 KiB).
- **Support buffer**: binding 0 of the stage set is used when the manifest has `support_buffer`
  (`size` 1600, `layout` `ryujinx-support`). Fill it with zeros except: offset 144
  `vec4(2/w, 2/h, 1, 1)` (viewport inverse), 160 `vec4(w, h, 1, 1)` (viewport size), and
  `render_scale` 1.0 in the first float of each 16-byte slot from 192 to 1344 (73 slots). Alpha test
  (offset 0) 0 means always pass; `is_bgra` (offset 16, 8 slots) 0 for RGBA targets.
- **Extra resource fields**: every resource has `vk_type` (`uniform_buffer`,
  `combined_image_sampler`, `sampled_image`, `sampler`, `uniform_texel_buffer`, `storage_image`,
  `storage_texel_buffer`, `storage_buffer`); use it for the descriptor type. Textures add
  `multisample`, `handle_type` (0 combined handle, 1 or 2 separate sampler word) and `separate`.
  SSBOs add `write`; `desc.offset` is in bytes (`c[0][0x110 + 16i]` gives `nvn_index` i).
- **Interface fields**: `inputs` and `outputs` entries add `type` (`float`, `int`, `uint`).
  `builtins_out` lists every member of the output block the module addresses, so `point_size` can
  appear; `builtins_in` can contain `base_instance` (needs `shaderDrawParameters`).
- **`capabilities` and `extensions`**: the SPIR-V capability numbers and extension names declared,
  for the device check at load time.
- **Programs Ryubing cannot translate** are `failed` with a `ryubing:` reason; a module that fails
  spirv-val is not published and is `failed` with a `build: ValueError: spirv-val` reason.

## 6. Change log

- v1 (2026-11-24): first version, written before the translator so task-12 can code against it.
- v1 (2026-11-24, additive, no reader change): `cf_mode`, `cf_fallback` manifest fields; required
  device features; rounding and oracle-compat notes. SSBOs (`LDG`/`STG` through NVN storage
  descriptors) are not emitted yet: those programs fail with reason `LDG` and use the fallback chain.
- v1 (2026-11-24, additive): SSBOs are emitted for `LDG` (read-only); manifest `ssbos` entries add
  `write` and `base_align`; `robustBufferAccess` joins the required features for SSBO modules.
- v1 (2026-11-24, additive): section 5, the Phase A cache `reuse-v1` (D-009) with bound program
  constants, the support buffer, `vk_type`, `source`, `capabilities` and `extensions`.
- v1 (2026-11-24, additive): geometry stage (`geometry` manifest object), multisample fetch
  (`dim` `2dms`), jump-table `BRX`, `VMAD`/`VMNMX`/`VSETP`; the bindless trace follows local-memory
  spills and predicated handle writes. Every program in the corpus now translates (0 failures).
- v1 (2026-11-25, additive): `TXQ`/`TXQ.B` size queries, `SUST.B` storage images (`images`
  entries), compute modules (workgroup size from the control section), `"multisample": true` on
  `2dms` textures, and index lines with `"source": "nvn_dump"` for the library programs the runtime
  dumps into `<shaders>/missing`. Changes every module with vertex outputs: position and generic
  outputs now start at (0, 0, 0, 1) instead of 0, so components a program never writes read as on
  hardware (the built-in blit VS writes only position .xy; w of 0 clipped everything).
