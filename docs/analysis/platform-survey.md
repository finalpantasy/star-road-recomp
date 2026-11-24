# Platform survey (task-3)

Date 2026-11-24. Inputs read-only: `A:\AI\Recomps\smrpg-work\v101\exefs\` and `...\v101\romfs\Data\`. Own parsers in `tools/survey/` (pure Python, run with `python -I`). Outputs in `A:\AI\Recomps\smrpg-work\analysis\survey\` (called SURVEY below). Nothing from the game is in this repo; only short identifiers are quoted.

Status key: FACT (checked in data, evidence named), INFERRED (follows from facts, not directly observed), UNKNOWN (next step given).

## Three facts that most affect the graphics strategy
1. **Shaders are precompiled NVN binaries with no source.** All 416 Shader objects (359 in `misc-shaders`, plus programassets and one root bundle) carry exactly one platform id (19) and an LZ4 blob of per-stage GPU programs plus constant-buffer reflection (`PGlobals`, `VGlobals`). No GLSL, no SPIR-V, no HLSL text. A GfxDevice-level path cannot reuse shader text; it needs either Maxwell binary translation or hand-written replacements for about 416 shaders (240 are Shader Graph, 21 are stock URP).
2. **The render pipeline is URP (SRP) with heavy Shader Graph use, on NVN.** Unity calls NVN through one imported entry point (`nvnBootstrapLoader`) and then a table of 535 `nvn*` procedures looked up by name. So the NVN surface is a closed, enumerable list (SURVEY `nvn_procs_main.txt`), and URP draw submission is fixed Unity code inside `main`.
3. **Textures are block-linear (GOB swizzled) ASTC 6x6 mostly, and the game is not touching Unity audio or video.** 10,593 of 13,883 Texture2D are ASTC 6x6, 1,191 ASTC 5x5, 1,317 DXT5; image data in `.resS` is padded and tiled (size larger than linear), so a PC path needs a deswizzler plus either an ASTC decoder or GPU ASTC (not universal on desktop GPUs) and conversion to BC7/RGBA. Audio is CRI ADX2 and movies are CRI USM with VP9, both outside Unity's own systems.

## Answers

### 1. Render pipeline: URP (FACT)
- `ScriptingAssemblies.json` lists `Universal.Runtime.dll`, `Universal.ShaderLibrary.dll`, `Universal.Shaders.dll` (`romfs\Data\ScriptingAssemblies.json`).
- Shader names in `misc-shaders.bundle` (parsed): 240 `Shader Graphs/*`, 65 `Hidden/*` (incl. `Hidden/Universal Render Pipeline/UberPost`, `FinalPost`, Terrain/Lit), 21 `Universal Render Pipeline/*`, 11 TextMeshPro, 3 `CriMana/*` (movie YUV to RGB, e.g. `CriMana/SofdecPrimeYuv`), plus MeshBaker, Graphy, Projector For LWRP. Evidence: `tools/survey/probe_shader.py`, SURVEY `bundle_classes.tsv`.
- Not yet read: the URP asset settings (renderer features, HDR, MSAA, render scale). UNKNOWN; next step: parse the URP asset MonoBehaviour in a bundle (class 114, type tree present) or read it from `programassets.bundle`.
- `boot.config`: `hdr-display-enabled=0`, `gc-max-time-slice=3`, `use-sleep-for-yield=25`, `use-legacy-fmod-priorities=1`, `enable-gfx-debugger=0`.

### 2. Shader storage format in Switch bundles (FACT, partial decode)
- Bundles are UnityFS v8, LZ4HC (comp type 3), Unity 2021.3.15f1, serialized file v22, target platform 38 (Switch), **type trees present in all 1,277 bundles** (so generic tree readers work; SURVEY `bundle_classes.tsv`).
- Shader object: `platforms=[19]`, `offsets`, `compressedLengths`, `decompressedLengths`, `compressedBlob`, plus `m_ParsedForm` (SerializedShader with names and properties). Blob = one LZ4 block per platform; decompressed it holds a table of sub-programs. Sample decoded: `CriMana/SofdecPrimeYuv`, 4,189 bytes compressed, 52,600 decompressed, 10 table entries; programs hold reflection (`PGlobals`, `VGlobals`, property names, keyword names such as `CRI_LINEAR_COLORSPACE`) and an opaque binary section. A header field reads 202012090 (looks like a compiler build stamp, INFERRED). No source text. Samples: SURVEY `shader_sample_*.bin`.
- UNKNOWN: exact sub-program table layout and the container of the NVN program binary (glslc output vs raw Maxwell). Next step: finish the table decode, then check whether the section carries a glslc `GpuProgram` header with control section plus Maxwell SASS (task-4 or a dedicated shader task).
- Variant count and total blob size not yet measured (359 blobs in `misc-shaders` are about 99 MB of the bundle).

### 3. Texture formats and swizzle (FACT)
- 13,883 Texture2D, 12.1 GB streamed in `.resS`. Formats: ASTC_6x6 10,593; DXT5 1,317; ASTC_5x5 1,191; RGBA32 474; DXT1 191; Alpha8 62; ASTC_4x4 23; ARGB4444 19; RGB24 11; BC7 2. Most common size 1024x1024 ASTC 6x6 (4,762). Mip counts: 11 (5,816), 10 (2,803), 1 (2,728). Full list: SURVEY `textures.tsv`.
- Swizzle: each Texture2D has a 92-byte `m_PlatformBlob`: header words (0, 0x20, mip-related word, 7, 0x10001), then per-mip offsets in 256-byte units. `m_CompleteImageSize` exceeds the linear size for every sampled ASTC and DXT5 texture (e.g. ASTC 6x6 1024x512 with 11 mips: stored 476,160 vs linear 314,992). That fits Switch block-linear (GOB) layout with padded mips. INFERRED, not yet round-tripped. Next step: write a deswizzler for one texture and compare against the linear decode (exact block height per mip comes from the blob and Maxwell GOB rules). Evidence: `tools/survey/probe_tex.py`.
- No AudioClip and no VideoClip objects exist in any bundle (count 0).

### 4. Audio middleware actually used (FACT)
- CRI ADX2 (Atom): `sound\Switch\` has `BGM2.acb/.awb`, `BGM3.acb/.awb`, `SE.acb/.awb`, `Stella_Sound.acf` plus `.h` cue ID headers (`@UTF` tables; `.awb` is `AFS2`). `main` embeds CRI Atom 2.24.182, AtomEx 2.24.4, ADX Decoder 1.00.08, CRI Base 2.50.16, Atom Unity 1.04.15, all built 2023-06-05 for Switch A57 (strings in `main` image).
- FMOD (Unity's built-in) is linked, with `use-legacy-fmod-priorities=1`, but no AudioClip assets exist, so FMOD carries no game audio content (INFERRED; confirm by checking whether the game calls `AudioSource`).
- Audio output goes through `nn::audio` (51 imports, audren/audout).
- UNKNOWN: waveform codec inside the AWBs (ADX, HCA, or other). Next step: read WaveformTable in an `.acb` (@UTF) and note the `EncodeType` per cue.

### 5. Movie codec (FACT)
- 46 files `movies\movie_001.usm` to `movie_046.usm`, 1.1 GB. CRI Sofdec USM containers (`CRID`, `@SFV`). Video header table (our `utf.py` reader): `mpeg_codec=9` (VP9 per CRI codec numbering; the source name is an `.ivf`), 44 files 1920x1080 and 2 files 1280x720, all 60 fps, no alpha; e.g. movie_001 has 2,070 frames. No `@SFA` audio streams in USM (audio is separate, ADX2). Table: SURVEY `movies.tsv`.
- Decode path on Switch: `main` imports `movie::CreateVp9DecoderConfigurationData`, `movie::Decoder`, `movie::Extractor` (31 `movie::` symbols) from `subsdk0`, so VP9 decodes via the Nintendo movie library (hardware). A PC port needs a USM demuxer (own) and a VP9 decoder (software or a library, which needs the owner's yes to add).

### 6. `data_tbl` and `script` formats
- `data_tbl\*.tbl` (126 files, 15 MB, plus `lang\*.tbl` 28 files, 7.9 MB of per-language message and menu text for chn, deu, eng, eng_us, eps, esp, esp_us, fra, fra_us, ita, jpn, kor, nld, twn): .NET `BinaryFormatter` (MS-NRBF) streams, header records name assembly `ExcelConverter, Version=1.0.0.0` and classes such as `ShopData` (`_shop_id`, `_area_id`, `_item_id`), `MonsterData` (`_attack`, `_defense`, `_speed`, `_resist_*`), `MessageData` (`m_id`, `m_chara_id`, `m_text`). FACT, from the first bytes and string scans. Parsing needs an NRBF reader (MS-NRBF is a public spec; small own parser feasible). Not written here.
- `script\*.bin` (454 files, 20 MB): all start `BIN\0`, little-endian, `0x0c` at offset 4, a record table at 0x28 (4-dword records, first values `0xc8`, `0x5898`, `0x8a0`, `0x20` in `battle.bin`). File names look like map IDs (`d01b1a`, `z01in9`) plus `battle.bin`. INFERRED container; opcode format UNKNOWN. Next step: use the decompiled script interpreter class from the IL2CPP dump (task-2) to get the exact record layout rather than guessing.

### 7. Save data hints
- Mount: `nn::fs::MountSaveData(name, Uid)`, `EnsureSaveData`, `IsSaveDataExisting`, `MountSaveDataReadOnly`, `Commit`; thumbnails via `nn::account::StoreSaveDataThumbnailImage` / `DeleteSaveDataThumbnailImage` (imports in SURVEY `nn_imports.tsv`). So the save is a per-user Horizon save partition, one commit per write.
- IL2CPP metadata names (`global-metadata.dat`, v29, unencrypted): `SaveData`, `_PlaySaveSlot`, `AutoSave`, `AutoSaveChecker`, `AutoSaveType`, `SaveMenuMain`, `EndingSaveMain`, `ComputeCRC`, `GetCRC32`, `Encrypted`, `EncryptionAlgorithm`, plus use of `BinaryFormatter`, `JsonUtility` and `PlayerPrefs` in the managed class library.
- UNKNOWN: which serializer holds the slots, whether it is encrypted, and the CRC scope. Next step: read the `SaveData` class fields and save/load methods from the IL2CPP dump.

### 8. `nn::` imports, grouped by service (AC2)
- Full list: SURVEY `nn_imports.tsv` (columns module, group, symbol; 1,660 rows over `main`, `subsdk0`, `sdk`). Classifier: `tools/survey/nnimports.py`. Raw dynsym import lists: SURVEY `nso\<module>.imports.tsv`.
- Counts for `main`'s 987 dynsym imports (all resolved against `sdk`/`subsdk0` at load time): nn-namespace 695; libc/libm 157; libc++ 74; `movie::` 31; runtime/ABI 19; other C++ 7; `nv::` 3; `nvn` 1.
- `main` nn groups: hid 124, os 100 (+20 C), friends 75, audio 51, fs 43, ec 39 (shop/eShop), account 26, socket 25 (+21 C), swkbd 24, irsensor 24, ssl 4 (+21 C), oe 20, nifm 9, ro 8, mem 8, ngc 7, core C 7, err 6, vi 5, time 5, diag 5, codec 4, aoc 4, util 3, pl 3, cstd 2, nsd 1, crypto 1. `nn::oe` includes `GetOperationMode`, `GetPerformanceMode`, `SetPerformanceConfiguration`, `SetCpuBoostMode`, focus handling and health-warning events, so docked/handheld mode switches reach the game.
- `npdm` service list (kernel-visible IPC services): acc:u0, aoc:u, appletOE, audin/audout/audren:u, bcat:u, bsd:u, caps:u, csrng, ectx:aw, friend:u, fsp-srv, hwopus, ldn:u, mii:u, nfc/nfp, ngct:u, nifm:u, nim:eca, nsd:u, nvdrv, pctl, pl:u, prepo:u, sfdnsres, time:u, vi:u.
- **AC2 check:** `main` has 987 dynsym imports (undefined symbols) and 987 rows in the TSV for `main`; subsdk0 660 and sdk 13, matching `nso\*.meta.json` counts.

### 9. `subsdk0` identity (FACT)
- It is the Nintendo SDK movie library: string `NintendoSDK_movie-14_3_1-Release`, exports `movie::*` (e.g. `movie::HLSHandler`, `movie::NnCodecConsumer`) over an Android stagefright/OMX stack (`android::*` 208 imports, NvMM codec strings for H.264 and VP8/VP9, `mkvmuxer`, 10,886 exports). Build ID `75c83c09...`. It imports 71 nn symbols (audio 19, os 28, sf 12, fs 4, lmem 3) and `nvnBootstrapLoader`. Evidence: SURVEY `nso\subsdk0.meta.json`, `.exports.tsv`, and strings.
- Other NSOs: `sdk` is the Nintendo SDK runtime (27,394 exports, mostly `nn::*`), `rtld` is the loader stub. `main` build ID `97f9664c...` (`nso\main.meta.json`). Segments of `main`: text at 0 (36.8 MB), ro at 0x2324000 (15.3 MB), data at 0x31c7000 (9.2 MB) plus 9.2 MB bss.
- `main.npdm`: ACI0 title ID `0100BC0018138000`, main thread priority 44, default core 0, main stack 16 MB.

### 10. NVN entry points (FACT)
- Exactly one imported NVN symbol: `nvnBootstrapLoader` (exported by `sdk` at 0x492f70, with `nvnBootstrapLoaderInternal`). `main` also imports `nv::InitializeGraphics`, `nv::SetGraphicsAllocator`, `nv::SetGraphicsDevtoolsAllocator` (nvdrv allocator setup).
- 535 `nvn*` procedure names exist as strings in `main` (CommandBuffer 156, TextureBuilder 44, SamplerBuilder 25, MultisampleState 23, TextureGet 21, QueueBuilder 19, MemoryPool 18, BlendState 15, TextureView 14, DepthStencil 13, WindowBuilder 12, DeviceGet 11, and others). Evidence list: SURVEY `nvn_procs_main.txt`. Unity resolves them through the loader at startup (INFERRED from the single import plus the name table). `nvnWindowSetPresentInterval` is in the table.
- Unity error strings reference `nvnProgramInitialize`, `nvnProgramSetShaders`, `nvnSyncWait`, and `switchNVNShaderPoolsGranularity`.
- Which of the 535 procs are actually called by the engine: UNKNOWN. Next step: static xref over the lifted code, or a runtime call log once a build runs.

### 11. Target frame rate and time step
- `TimeManager` (in `globalgamemanagers`, class 5, 16 bytes): fixed timestep **0.02 s (50 Hz)**, maximum allowed timestep 0.1 s, time scale 1.0, maximum particle timestep 0.03 s. FACT.
- `QualitySettings`: three named levels Low, Medium, High, current index 5 field present (layout partly decoded, `tools/survey/probe_q.py`). Per-level `vSyncCount` not reliably extracted: UNKNOWN.
- `Application.set_targetFrameRate` and `QualitySettings.set_vSyncCount` exist in the IL2CPP metadata, so the game or a plugin may set them at runtime, but the value is code, not data. UNKNOWN. Next step: find the call sites in the IL2CPP dump (task-2) and read the constants, then confirm on the first run (a swap-interval log from `nvnWindowSetPresentInterval`).
- Prior: CRI movies are 60 fps; Unity's default Switch swap interval is 1 (60 Hz) when vSync count is 1, 30 fps with 2. Not a fact for this game.
- Player settings: default screen 1920x1080 (from the 798-byte PlayerSettings object), company "Nintendo" (hardcoded SDK strings), product string is a build tag. `hdr-display-enabled=0`.

### 12. Other facts the recompiler will meet
- Bundles: 1,277 UnityFS, 5.2 GB, all LZ4HC, 711 in `d_assets_`, 530 scene bundles in `d_scenes_\scenes`, 28 in `fonts_assets_`, 1 root. Class counts over all bundles (SURVEY `bundle_classes.tsv`): GameObject 463,867, Transform 450,476, MeshFilter 346,079, MeshRenderer 345,876, MonoBehaviour 41,310, Mesh 15,083, Texture2D 13,883, RectTransform 13,391, Material 8,702, ParticleSystem 7,431, Animator 5,722, AnimationClip 5,529, Sprite 2,271, SkinnedMeshRenderer 1,580, Shader 416, TextAsset 34, Font 30, Camera 51. The 530 scene bundles dominate counts of static meshes.
- Asset loading is Addressables (`aa\catalog.json` 8.5 MB, `settings.json`).
- `globalgamemanagers` has no type trees (player data), 18 objects; the bundles do have type trees.

## Method and reproduction
- NSO: `tools/survey/nso.py <nso> <outdir>` (LZ4 block decode, MOD0 and dynamic section, dynsym via DT_GNU_HASH). `nnimports.py` groups imports.
- Unity: `unityfs.py` (UnityFS reader, SerializedFile v22 metadata and type trees, generic tree reader), `scan_bundles.py`, `scan_textures.py`, `probe_shader.py`, `probe_tex.py`, `probe_ggm.py`, `probe_q.py`.
- CRI: `utf.py` (@UTF table reader), `scan_usm.py`, `probe_usm.py`.
- All scripts take paths as arguments, read inputs read-only, write only under SURVEY. No packages installed, nothing downloaded.

## Open items for follow-up (not done here)
1. Finish shader blob decode and check the NVN program container (needed for any Maxwell translation decision).
2. Deswizzle one texture to confirm block-linear layout and block height.
3. URP asset settings and per-level vSync/targetFrameRate from the IL2CPP dump.
4. AWB codec per cue; NRBF reader for `.tbl`; script `.bin` record layout.
