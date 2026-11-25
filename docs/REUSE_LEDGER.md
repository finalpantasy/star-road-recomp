# Reuse ledger (D-009)

Reused code lives only in the gitignored `reuse/` folder (and on A:). It is never committed or pushed. Each row is removed when our own rewrite replaces it and passes the same tests with the reused version as the oracle. Public release requires this table to be empty.

| Piece | Source (project, commit, path) | Origin license | Used by | Rewrite card | Status |
|---|---|---|---|---|---|
| Phase A shader cache: the Ryubing shader translator, run (not copied) through the Snap ShaderHarness `--nvn-serve` binary; our own driver `reuse/shaders/ryu_cache.py` moves its descriptors to the docs/SHADER_CACHE.md layout. Output `A:\AI\Recomps\smrpg-work\shaders\cache\reuse-v1\` | ryubing-ryujinx 4d5a19d3 `src/Ryujinx.Graphics.Shader`; new-pokemon-snap 1e7a982 `scripts/shader_harness/Program.cs` | MIT (Ryubing); Snap harness is the owner's own code | task-12 (runtime shader cache) | task-10 Phase B (`tools/shaderxlate/`, validated against this cache) | Active |
| Movie decode: FFmpeg libavcodec and libavutil DLLs, loaded at run time (D-011, D-012). Development uses the MSYS2 build in `A:\msys64\ucrt64\bin` | FFmpeg (MSYS2 mingw-w64-ucrt-x86_64-ffmpeg) | LGPL 2.1+ or GPL, depending on the build (check `avcodec_license()`) | task-21 (nn::movie HLE) | none yet (own VP9 decoder later) | Active |
