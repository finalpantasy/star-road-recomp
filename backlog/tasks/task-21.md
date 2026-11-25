---
id: task-21
title: Movies: HLE nn::movie with FFmpeg (LGPL DLL) so the intro movies play and the title screen appears (D-011)
status: Ready
---
## Problem before solution
Our build runs 45 s without a crash, but the screen goes black, then flat gray or white, once CRI opens `movie_017.usm` (about 12 s) and `movie_001.usm` (about 27 s). Nothing decodes the video: `subsdk0` (`nn::movie`, Android stagefright plus nvdec) runs lifted and its `/dev/nvhost-nvdec` ioctls fail, or with `SMRPG_NVDEC_OK=1` succeed with no output. The title screen sits behind these movies.
## Outcome and scope
The intro movies decode and show, and the game reaches the title screen in a hidden muted run on our own runtime. In scope: an HLE of the `nn::movie` imports the game uses (docs/RECOMPILER.md section 4.6 boundary), VP9 (and Opus if the movies carry it in this path) through libavcodec loaded from a separate LGPL-only FFmpeg DLL, the movie texture upload (pitch-linear output, stride 0x1e00 seen; `UploadTexture` assumes block-linear), and a build script for that DLL. Out of scope: movie audio to a real device (runs stay muted), upscaling options (later, D-004), CRI USM demuxing if CRI already does it in lifted code.
## Dependencies and ownership
Depends on D-011, task-13 (HLE services), task-12 (NVN textures). Owns `runtime/hle/` files it adds for `nn::movie`, `tools/ffmpeg/` (build script), this card. Changes to `runtime/nvn/` texture upload or `runtime/hle/hle_gfxsvc.c` are allowed but listed in the Log. FFmpeg source and builds live in `A:\AI\Recomps\ffmpeg\` only.
Execution: subchat smrpg-movies, Sonnet high.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | LGPL-only DLL (optional this round; owner OK to include FFmpeg, MSYS2 DLLs allowed for development) | Built from a pinned FFmpeg release by `tools/ffmpeg/build.py` with only the decoders and parsers needed; `configure` output shows no `--enable-gpl`, `--enable-nonfree`; license reported LGPL | build log on A:, `avcodec_license()` output | |
| AC2 | nn::movie HLE | Every `nn::movie` import the game calls on the boot path is handled (strict mode: no unknown call); nvdec ioctls no longer reached; `SMRPG_NVDEC_OK` not needed | `hle_boot.log` grep | |
| AC3 | Frames decode | movie_017 and movie_001 decode with 0 decode errors; frame count matches the USM header | log counters | |
| AC4 | Pictures right | Captured movie frames show the movie (not black, gray or white); compared by eye with the baseline's frames at the same moments (`A:\AI\Recomps\smrpg-work\captures\baseline\`) | PNGs on A: | |
| AC5 | Title screen | A hidden muted run reaches the title screen (logo over Bowser's Keep) | PNG on A:, compare with `captures\baseline\frame_00400.png` | |
| AC6 | No regressions | `tests/nvn/run_nvn_tests.py`, `tools/game_check.py run` pass | outputs | |
## Human gates
None for the code. Appearance stays the owner's call (ART_DIRECTION).
## Recovery and return condition
Everything from FFmpeg stays on A:. Return once at done or blocked.
## Approach and commands
- Build: `python -I tools/rtprep/build_game.py`. Run: `python -I tools/rtprep/run_game.py --seconds 60 --env SMRPG_CAPTURE_DIR=<dir> --env SMRPG_CAPTURE_EVERY=N` (hidden, BelowNormal, null audio sink). Log: `A:\AI\Recomps\smrpg-work\build\logs\hle_boot.log`. `SMRPG_FSLOG=1` logs file paths.
- Reference (ideas only, D-009/D-006): the baseline `A:\AI\Recomps\smrpg-elkiro` has CRIWARE VP9 decode through FFmpeg and `runtime/hle/movie.c`; read it, write our own.
- MSYS2 at `A:\msys64` has an FFmpeg package; it is likely a GPL build, so it is not used for shipping. Build a minimal LGPL one.
## Log and handoff
