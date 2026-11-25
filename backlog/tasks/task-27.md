---
id: task-27
title: Mix audren voices to the opt-in SDL audio output
status: Verified
---
## Observed behavior

The audren HLE shapes update responses and signals the frame event. Its voice and sink statuses carry no rendered samples. The SDL3 output path exists for audout. In `codex_t26_02`, the guest queried the audren device, opened audout at 48 kHz with six channels, and made 30,826 buffer append calls. The trace has no renderer open or update request. The source of any missing sound still needs confirmation.

## Outcome and scope

Validate the renderer update layout against the revision and voice formats observed in a local trace. Mix the supported voices into stereo and send the result through the existing SDL output path when audio is enabled. Keep hidden runs on the dummy driver and support an offline PCM capture for verification. Record unsupported voice formats or routing separately when they appear in a trace.

## Dependencies and ownership

Builds on task-13 audio HLE and task-22 SDL output. Owns the audren code in `runtime/hle/hle_audio.c`, any new mixer code and tests, and the minimal platform wiring needed to use the existing output. The lead owns the shared build and runtime test slot.

## Acceptance and evidence

| ID | Criterion | Check | Evidence | Result |
|---|---|---|---|---|
| AC1 | Mixer produces expected samples | Compare synthetic voices against known PCM output for gain, channel routing, clipping, and buffer completion. Exercise malformed and out-of-range buffers. | Unit test output | PASS (mixer core only: `python -I tests/audio/run_audio_tests.py` audren_mix 166 checks, 0 failed; hwopus 323 checks, 0 failed) |
| AC2 | Guest voices reach the output | Run hidden with the dummy driver and capture samples before device submission. Confirm valid PCM, expected timing, and voice completion in the guest. | PCM measurements and renderer log | PASS for the path the guest really uses (audout fed by hwopus decode; the guest has no audren renderer, so no audren voices exist to complete). `A:\AI\Recomps\smrpg-work\captures\t27_open\analysis.json`: 114 of 138 s active, signal on channels 0 and 1, 15 libopus decoders, 0 errors, 0 unknown IPC. `--audio` dummy-device run `captures\t27_open_dev\analysis.json`: 37 of 65 s active, peak 23832, 0 clipped, 0 errors. Logs in `build\logs\t27_open.log`, `t27_open_dev.log` |
| AC3 | Default run stays muted | Boot with default options and verify no host audio device opens | Platform test and runtime log | PASS: `python -I tests/platform/run_platform_tests.py` 392 checks, 0 failed ("audio (muted by default)"); the `t27_open` run log has no audio device line without `--audio` |
| AC4 | Playback sounds correct | Owner listens for timing, missing voices, and distortion in a visible play session | Owner feedback | NOT_RUN |

## Reproduction

Capture audout PCM through the dummy driver and check whether it contains the expected signal. Find a guest scenario with an audren renderer open and update request before selecting the first mixer formats. Keep trace summaries and captures on A:. Run the platform audio tests and `python tools/game_check.py run --root .` after wiring the output.

In the hidden `codex_t27_audio_probe` run, `--audio` opened the SDL dummy device at 48 kHz with six channels. The guest made 2,506 buffer append calls and used zero unknown IPC commands. PCM content and audible output remain unverified.

## Log

### 2026-11-25 smrpg-audio worker (interim)

- Finding: the guest never opens an audio renderer. In `codex_t26_02` (175 s) the audren manager only answers `GetAudioDeviceService` (2 calls); there is no OpenAudioRenderer and no RequestUpdate. The game mixes in its own engine and appends PCM to audout (30,826 appends) after decoding Opus streams through **hwopus**: 15 `OpenHardwareOpusDecoderForMultiStreamEx` (cmd 6) and 42,266 `DecodeInterleavedForMultiStream` (cmd 9) calls, first at 42.3 s. The old hwopus HLE was a null decoder (silence for every packet), so that is where the sound was lost, not audren.
- Done: real hwopus decode through libopus (`runtime/audio/hwopus_dec.h`, DLL loaded at run time like FFmpeg, `libopus-0.dll` copied beside the exe by `build_game.py`), env-gated audout capture `SMRPG_AUDIO_DUMP=<wav>` with per-5-s measurement lines (`runtime/audio/pcm_dump.h`, in `hle_audio.c`), a unit-tested audren mixer core (`runtime/audio/audren_mix.h`, not wired to RequestUpdate: no guest voice layout exists to validate against; ADPCM and other formats refused, not guessed), analysis and capture scripts in `tests/audio/`.
- Unit tests: `python -I tests/audio/run_audio_tests.py`: hwopus 323 checks, audren_mix 166 checks, 0 failed.
- Captures (A:, never in Git): `t27_open` (no `--audio`): 138 s, 114 s active, audio/wall 0.95. `t27_open_dev` (`--audio`, dummy driver): 64 s, 37 s active, audio/wall 0.674 (paced by the device clock, as expected), first signal at 11 s (movie), music after.
- Open for the lead: the audren mixer is not wired (no guest uses audren). `tests/platform/run_platform_tests.py` launches the exe directly and does not take the heavy slot. `tests/audio/*` and `tests/rtprep/test_heavy_lock.py` are not yet listed in `tools/check-config.json` (not mine). AC4 stays with the owner.
- Heavy-slot lock: `tools/rtprep/heavy_lock.py`, used by `build_game.py` and `run_game.py`; `python -I tests/rtprep/test_heavy_lock.py` 17 checks, 0 failed. Documented in `docs/PLATFORM.md`.

### 2026-11-25 Claude lead
Verified: audio tests 489 checks and heavy_lock 21 checks rerun by the lead, 0 failed; `t27_open_dev/audout.wav` is 63.7 s, 6 channels at 48 kHz, RMS per 5 s rising from 0 to about 2700 with the movie and music (not silent). libopus added to REUSE_LEDGER. Open: AC4 owner listening; the audren mixer core stays unwired until a guest opens an audio renderer.
