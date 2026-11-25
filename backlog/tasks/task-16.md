---
id: task-16
title: In-game overlay menu inside the game settings menu
status: In progress
---
## Problem before solution
Owner to-do (2026-11-24): an in-game overlay menu integrated into the actual game settings menu.
## Outcome and scope
Our options (resolution, frame rate, upgrades, QoL, controls) appear as extra pages inside the game's own Settings menu, in its style, plus a hotkey overlay for anything the game menu cannot host. Controller and keyboard.
## Dependencies and ownership
Starts after our own runtime renders its first frame (owner, 2026-11-24; met 2026-11-25). Needs task-12 (NVN), task-13 (HLE), task-22 (SDL3 window and input). Drawn with Dear ImGui (MIT, owner yes 2026-11-25, D-012), fetched at a pinned release into `A:\AI\Recomps\thirdparty\imgui\` by a committed script, never committed itself; styled to match the game's menus.
Owns `runtime/platform/overlay/` (ImGui setup, its Vulkan and SDL3 backends, our options pages), `tools/thirdparty/fetch_imgui.py`, `config/options.default.ini`. Draws through `PlatOverlayDraw(...)`, the hook task-23 adds in `QueuePresent`; does not edit `QueuePresent` itself. Input comes from `runtime/platform/` (SDL events go to ImGui while the overlay is open and are kept from the game). First round: a hotkey overlay (F1 and a controller combo) with real working options; hosting pages inside the game's own Settings menu is the next round.
Execution: subchat smrpg-overlay, Sonnet high; design review by the owner.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Overlay opens and closes | F1 and a controller combo (scripted through `SMRPG_INPUT`) toggle it in a hidden run; the game gets no input while it is open | captures with and without the overlay, log | PASS (technical, 2026-11-25): combo L+R+MINUS and the scripted `F1` token open it; `overlay: while open, 1499 pad samples, 126 carried input; all hidden from the game`; captures `A:\AI\Recomps\smrpg-work\t16c1\`. The real keyboard F1 calls the same `overlay_toggle` (a hidden run takes scripted input only, so the key itself is not exercised). Look and feel: owner. |
| AC2 | Options work | Every page item reads and writes `config/options.ini` (upscale mode from task-23 when present, frame rate cap, window mode, input map path, audio on and volume); changes apply live where the runtime supports it, else marked "on restart" | unit test of the options store, captures | PASS: `tests/platform/test_options.c` 135 checks (in `run_platform_tests.py`); every item changed live in a scripted run (log `t16_ac3.log`); saved file checked; fps cap measured at 90.0 fps (`t16_ac3.log`); audio enabled marked "on restart"; window mode applies only with the window shown (`--window`, owner runs that), so only its stored value is shown here. |
| AC3 | Controller only | Every item is reachable and changeable with the d-pad and A/B alone (scripted run) | log of the scripted run, captures | PASS: one scripted run of d-pad, L/R, X and B presses (`ac3_script.txt`) visits all 5 pages and all 10 rows and changes every value row; reset-all (A twice) and "Close menu" in `t16_ac3b.log`. No keyboard or mouse used. |
| AC4 | Faithful when closed | With the overlay closed, the present is byte-identical to the build without it at the same frame | hashes | PASS: `ref-blit=equal` at frames 290, 520 and 600 (closed, before and after an open) and `DIFFERENT` only at 350 and 470 (open). Game frames are not deterministic run to run (the same frame hashes differ between two runs), so equality is shown against the plain blit redone in the same command buffer, not against a second build. `PlatOverlayDraw` returns on its first line when closed. |
| AC5 | No regressions | platform, NVN tests, game_check | outputs | see Log |
| AC6 (next round) | Pages inside the game's own Settings menu with controller only | Run | Capture | NOT_RUN |
## Human gates
The owner approves look and feel (docs/ART_DIRECTION.md); technical PASS does not approve appearance.
## Recovery and return condition
Return once at done or blocked. Runs hidden and muted.
## Approach and commands
Study the baseline recomp's options menu and the game's own settings screens (captures on A:) first. Our own code; D-009 for anything reused.

Round 2 starts from task-26's verified gameplay route. Confirm controller navigation, expose the existing runtime options through a Settings entry, and test that closing it returns focus to the guest menu. Keep the standalone hotkey available. Record a controller-only capture from Settings through an option change and back. Codex worker `/root/settings_integration` is mapping the existing hook points and evidence needed. The lead owns changes and runs.

`codex_t16_r2_01` loaded a copy of the opening autosave. Menu inputs in the first two probes missed the intended gameplay window. The third probe used an incomplete input script and was stopped. In `codex_t16_r2_04`, a corrected script loaded the same save and reached the main menu with Plus. Present 6184 shows the System entry. The guest log confirms both the save load and controller input.

The discovery worker identified candidate guest System menu methods in the local IL2CPP analysis. The runtime's existing overlay hook receives frame and input data, with no guest menu identity. Confirm the Settings state and selection action in a runtime trace before wiring a one-shot open request. The controlled `codex_t16_r2_05` probe loaded the room late and reached its watchdog before the scheduled navigation. AC6 still needs the guest Settings route and overlay handoff.
## Log and handoff
Record measured numbers and screenshots on A:.

### Round 1 result (smrpg-overlay worker, 2026-11-25)
- Done: AC1 to AC4. AC5: see below. AC6 not started. Nothing committed.
- Code: `runtime/platform/overlay/` (`options.c/.h` the table-driven store, `overlay_apply.cpp` input, filter, pacing and apply, `overlay.cpp` the ImGui menu and the strong `PlatOverlayDraw`), `tools/thirdparty/fetch_imgui.py` (pinned v1.93.0, sha256 checked), `config/options.default.ini`, hooks in `plat_core.c`, `plat_audio.c` (volume), `pad.c` (token `F1`), additive blocks in `runtime/CMakeLists.txt` and `tools/rtprep/build_game.py`, `docs/PLATFORM.md` "Options overlay".
- Evidence on A: `A:\AI\Recomps\smrpg-work\t16\` (captures `ac1\`, `cap1\`, scripts `ac3_script.txt`), logs `A:\AI\Recomps\smrpg-workuild\logs	16_*.log`.
- Added at the lead's request (owner, 2026-11-25): Gameplay page with `gameplay.unlock_postgame = off | on` (task-24), shown "on load" (applies on save load or new game). Read from game code with `overlay_unlock_postgame()` or `overlay_option(OPT_UNLOCK_POSTGAME)` (`runtime/platform/overlay/overlay.h`; documented in docs/PLATFORM.md). The Hard difficulty options were dropped (owner cancelled). Run: `t16_gp.log`, capture `t16\gp\present_000440.png`.
- AC5 result: platform tests PASS (392 checks, options 144 checks). NVN tests: nvn_unit, nvn_gpu and nvn_cache pipelines PASS; the cache sweep timed out twice at 245 s (exit 124) while every sweep line showed 0 failures: the shader-cache file reads slowed from 2 s to 140 s per 4000 files with 100+ python processes and Defender active on the PC (52 s when quiet), not a test failure and no NVN file is mine. Re-run it when the PC is quiet; the committed NVN report now holds the timed-out run. `game_check`: FAIL at the lifter check: runtime boot test (mock game) fails linking: `hle_fs.c:478` (`fsys_open_file`, `save_unlock_view` calling `postgame_is_save_name`) has an undefined symbol because the mock-game link lacks the task-24 postgame sources (another worker's uncommitted edit), not in my files; the instruction oracle and lift smoke pass.
- Known: ImGui's Vulkan backend uploads its font texture with its own queue submit and wait idle (first open, new glyphs); the hook contract says record only. Documented in PLATFORM.md.
- Known: look is a first pass in the game's menu colors for the owner to judge (docs/ART_DIRECTION.md).
