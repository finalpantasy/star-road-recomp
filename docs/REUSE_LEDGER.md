# Reuse ledger (D-009)

Reused code lives only in the gitignored `reuse/` folder (and on A:). It is never committed or pushed. Each row is removed when our own rewrite replaces it and passes the same tests with the reused version as the oracle. Public release requires this table to be empty.

| Piece | Source (project, commit, path) | Origin license | Used by | Rewrite card | Status |
|---|---|---|---|---|---|
| Phase A shader cache: the Ryubing shader translator, run (not copied) through the Snap ShaderHarness `--nvn-serve` binary; our own driver `reuse/shaders/ryu_cache.py` moves its descriptors to the docs/SHADER_CACHE.md layout. Output `A:\AI\Recomps\smrpg-work\shaders\cache\reuse-v1\` | ryubing-ryujinx 4d5a19d3 `src/Ryujinx.Graphics.Shader`; new-pokemon-snap 1e7a982 `scripts/shader_harness/Program.cs` | MIT (Ryubing); Snap harness is the owner's own code | task-12 (runtime shader cache) | task-10 Phase B (`tools/shaderxlate/`, validated against this cache) | Active |

## Third-party libraries (D-012: may stay permanently)
General libraries, not code from other recomps. Binaries are never committed; release ships them beside the exe with their license files.

| Library | Version and source | License | Used by | Notes |
|---|---|---|---|---|
| FFmpeg (libavcodec, libavutil) | 9.0.2, sha256 pinned, built by `tools/ffmpeg/build.py` into `A:\AI\Recomps\ffmpeg\install` (VP9 decoder and parser only) | LGPL 2.1 or later (`avcodec_license()` checked; no gpl, nonfree or version3) | task-21 movie decode (D-011) | Loaded at run time by `av_load()` in `runtime/hle/hle_movie.c`; `SMRPG_FFMPEG_DIR` overrides |
| SDL3 | MSYS2 mingw-w64-ucrt-x86_64-sdl3 3.4.20 in `A:\msys64\ucrt64` (development) | zlib (no notice needed in binaries) | task-22 platform layer | Owner yes 2026-11-25 |
| Dear ImGui | pinned release fetched to `A:\AI\Recomps\thirdparty\imgui\` by a committed script | MIT (notice in the release notices file) | task-16 overlay | Owner yes 2026-11-25; not fetched yet |
| AMD FidelityFX FSR | pinned release fetched to `A:\AI\Recomps\thirdparty\fsr\` by a committed script | MIT (notice in the release notices file) | task-23 upscaling | Owner yes 2026-11-25; not fetched yet |
| libiconv | MSYS2 `libiconv-2.dll` (dependency of the MSYS2 SDL3 build), copied beside the exe by `build_game.py` | LGPL 2.1 or later (notice and source pointer at release) | task-22 (through SDL3) | Drops out if SDL3 is built without iconv at release |
