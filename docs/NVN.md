# NVN on Vulkan (task-12)

Our own implementation of the NVN graphics API on Vulkan, in `runtime/nvn/` (C++17 plus one C file, clang). The game calls NVN through `nvnBootstrapLoader`; the runtime hands it synthetic guest addresses that land in the Ctx adapters here. Tests live in `tests/nvn/`. D-008 applies: Snap's NVN layer was read for lessons only, no code was copied.

## What is implemented

| | |
|---|---|
| Procs in the table | 535 names (the whole NVN proc list), generated into `nvn_sigs.inc` by `tools/nvngen/gen_sigs.py` |
| Implemented | 332 |
| Logged stubs | 203 (log once, return 0, with a reason) |
| Statically referenced by the game | 265 distinct names, all present in the table (the task card says 277; the measured list from the import table has 265) |
| Of those 265, implemented | 263 |
| Of those 265, stubbed with a reason | 2 (listed below) |

Used-but-stubbed procs:

| Proc | Reason |
|---|---|
| `nvnCommandBufferBindStorageBuffersDeferred` | Deferred storage buffer binding has no Vulkan equivalent worth emulating until a translated shader uses it. |
| `nvnCommandBufferSetViewportSwizzles` | NV_viewport_swizzle is not available on the Vulkan target; the game only sets identity. |

Every other stub is outside what the game's code reaches. `nvn_unit` checks the table on every run: no duplicate names, every proc has a thunk, every implemented proc's registered signature equals the generated one, every stub has a reason, and the used list (built from the game's import table, kept on A:, never in Git) is fully covered.

Counts by object class (regenerate with `python -I tools/nvngen/gen_doc.py`):

<!-- proc-table:begin -->
| Object class | Procs | Implemented | Logged stubs |
|---|---|---|---|
| BlendState | 15 | 7 | 8 |
| Bootstrap | 1 | 0 | 1 |
| Buffer | 11 | 11 | 0 |
| BufferBuilder | 7 | 7 | 0 |
| ChannelMaskState | 3 | 2 | 1 |
| ColorState | 7 | 5 | 2 |
| CommandBuffer | 156 | 74 | 82 |
| DepthStencilState | 13 | 7 | 6 |
| Device | 32 | 14 | 18 |
| DeviceBuilder | 3 | 2 | 1 |
| Event | 6 | 0 | 6 |
| EventBuilder | 5 | 0 | 5 |
| MemoryPool | 10 | 9 | 1 |
| MemoryPoolBuilder | 8 | 8 | 0 |
| MultisampleState | 23 | 8 | 15 |
| PolygonState | 9 | 5 | 4 |
| Program | 8 | 5 | 3 |
| Queue | 14 | 9 | 5 |
| QueueBuilder | 19 | 7 | 12 |
| Sampler | 15 | 15 | 0 |
| SamplerBuilder | 25 | 24 | 1 |
| SamplerPool | 8 | 8 | 0 |
| Sync | 6 | 4 | 2 |
| Texture | 31 | 25 | 6 |
| TextureBuilder | 44 | 35 | 9 |
| TexturePool | 8 | 8 | 0 |
| TextureView | 14 | 13 | 1 |
| VertexAttribState | 5 | 3 | 2 |
| VertexStreamState | 5 | 2 | 3 |
| Window | 12 | 9 | 3 |
| WindowBuilder | 12 | 6 | 6 |
| **Total** | **535** | **332** | **203** |
<!-- proc-table:end -->

## Design

- **Guest objects** are blobs in guest memory: a 16 byte header `{magic, pad, impl*}` and the host object behind it. Builders and state objects are plain data in place. `NVNtexture` is 0x80 bytes (checked by `nvn_unit` and by canaries in `nvn_gpu`), `NVNtextureView` is 16 bytes.
- **Procs** are registered with `NVN_API(name, ret, params)`. The macro records the native function, its signature and a Ctx adapter that follows AAPCS64: integers and pointers come from `x[]`, floats from `v[]`, results go to `x0`/`v0`, a bool is the low byte, 32 bit arguments ignore the upper half of the register.
- **Memory pools** import the guest pages as host pointers (`VK_EXT_external_memory_host`) and get a virtual GPU address (`0x40'0000'0000` base). A buffer address resolves back to a VkBuffer plus offset at replay time.
- **Command buffers** record into a command list; `nvnQueueSubmitCommands` replays it synchronously into one Vulkan command buffer (dynamic rendering, GENERAL image layouts, a timeline semaphore for sync). Resource preparation (layout changes, texture uploads through task-11, dummy images) happens before a render pass begins, never inside one.
- **Pipelines** are built on demand and cached by state. Depth mode `NEAR_IS_MINUS_W` uses `VK_EXT_depth_clip_control`; the `UPPER_LEFT` window origin is a negative-height viewport.
- **Textures** upload through `runtime/gfx/texture` (task-11): block-linear deswizzle, ASTC and BC decode where the GPU lacks the format. Separate texture and sampler handles follow task-10 (`docs/SHADER_CACHE.md` v1): texture id and sampler id come from the guest's descriptor pools, `id >= 256` (the console reserves the first 256).
- **Shaders** come from the shader cache (task-10): the key is the SHA-256 of the stage bytes at the program's data address. A draw whose stage is missing or failed to translate is skipped and logged once.
- **Presentation** creates its own window at (-32000, -32000) as a `WS_POPUP`, never shown and never activated, and presents a blit of the game's display texture to a swapchain on it. With no surface (or `NVN_PRESENT` unset) the display textures are simply the target.
- **Teardown** destroys pipelines, layouts, shader modules, dummies and scratch buffers before the device, so the validation layer reports no leaks.

## Registering with the runtime

The runtime (task-5) owns the rest of `runtime/`. NVN needs three additions to its build, none to its code:

1. `add_subdirectory(nvn)` in `runtime/CMakeLists.txt` (defines `smrpg_nvn` and `SMRPG_NVN_REGISTER_SOURCE`).
2. Append `${SMRPG_NVN_REGISTER_SOURCE}` to the `smrpg_boot` sources. `nvn_register.c` uses the `HLE` macro, so the import `nvnBootstrapLoader` registers itself at load time. `nvn_rt.h` is the C interface.
3. Link `smrpg_nvn` into `smrpg_boot` (`SMRPG_EXTRA_LIBS`). The three `LinkAnchor*` functions in `nvn_core.h` keep the implementation objects in the static library.

Guest flow: `nvnBootstrapLoader("nvnDeviceInitialize" | "nvnDeviceGetProcAddress")` returns a slot address; everything else comes from `nvnDeviceGetProcAddress(device, name)`. In host and test mode (no resolver set) `ResolveProc(name)` returns the native function pointer, which is how the tests call the same procs.

## Options

| Variable | Effect |
|---|---|
| `NVN_VALIDATION=1` | Enable `VK_LAYER_KHRONOS_validation` (errors are counted and logged). |
| `NVN_LAYER_PATH` | Folder with the validation layer (`VkLayer_khronos_validation.dll` and its json). |
| `NVN_PRESENT=1` | Load the surface extensions so a window can present. Off: offscreen only. |
| `NVN_LOG` | Log level. |

Host code can also call `Configure(VkInitOptions{...})` and `SetWindowHidden(true)` before the device is created; the tests do.

Defaults the game overrides explicitly: depth mode `NEAR_IS_ZERO`, window origin `UPPER_LEFT`.

## Known limitations

- `ClearColor` with a partial channel mask clears all channels (logged once).
- A `LOWER_LEFT` origin flips the image vertically at present, not in the render path.
- Mirror-clamp wrap modes map to the nearest Vulkan mode.
- Alpha test is not emulated (the game's translated shaders must discard instead).
- `ReportCounter` is a no-op; counter memory stays zero.
- Vertex divisors above 1 and texel buffers are not supported.
- A texture re-uploads only after `nvnMemoryPoolFlushMappedRange` or `nvnTextureWriteTexels`.
- Packaged texture layout bits are stored, not interpreted.

## Tests and evidence

`python -I tests/nvn/run_nvn_tests.py` builds, runs both tests at BelowNormal priority and writes `tests/nvn/reports/NVN_TEST_REPORT.md`.

- `nvn_unit`: struct sizes (`NVNtexture` 0x80, view 16 bytes, builders), canaries around the view proc, the proc table checks above, the Ctx adapter ABI (upper register garbage, bool, float, 64 bit and pointer returns), format mapping (RGBA4 to `A4B4G4R4`, RGBA8, DEPTH32F), depth enum values.
- `nvn_gpu`: on the machine's GPU with validation on and a hidden window. Clear and readback; textured quad with separate texture and sampler handles and a uniform buffer; RGBA4 texel order (R in bits 0..3, A in bits 12..15) read back as pixels; depth range for both depth modes (z outside 0..1 is clipped under `NEAR_IS_ZERO`, depth stored as z; `NEAR_IS_MINUS_W` maps -1..1 to 0..1); fences, `QueueWaitSync`, buffer to buffer and texture to buffer copies, uniform buffer update; three presented frames on a window that stays hidden; zero validation errors and warnings.

Generated shaders, images and logs go to `A:\AI\Recomps\smrpg-work\nvn\`. The tests need a Vulkan 1.3 device and the SDK's `glslc`; without them `nvn_gpu` exits 3 (NOT_RUN).
