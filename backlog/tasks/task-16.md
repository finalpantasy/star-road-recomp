---
id: task-16
title: In-game overlay menu inside the game settings menu
status: Ready
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
| AC1 | Overlay opens and closes | F1 and a controller combo (scripted through `SMRPG_INPUT`) toggle it in a hidden run; the game gets no input while it is open | captures with and without the overlay, log | |
| AC2 | Options work | Every page item reads and writes `config/options.ini` (upscale mode from task-23 when present, frame rate cap, window mode, input map path, audio on and volume); changes apply live where the runtime supports it, else marked "on restart" | unit test of the options store, captures | |
| AC3 | Controller only | Every item is reachable and changeable with the d-pad and A/B alone (scripted run) | log of the scripted run, captures | |
| AC4 | Faithful when closed | With the overlay closed, the present is byte-identical to the build without it at the same frame | hashes | |
| AC5 | No regressions | platform, NVN tests, game_check | outputs | |
| AC6 (next round) | Pages inside the game's own Settings menu with controller only | Run | Capture | NOT_RUN |
## Human gates
The owner approves look and feel (docs/ART_DIRECTION.md); technical PASS does not approve appearance.
## Recovery and return condition
Return once at done or blocked. Runs hidden and muted.
## Approach and commands
Study the baseline recomp's options menu and the game's own settings screens (captures on A:) first. Our own code; D-009 for anything reused.
## Log and handoff
Record measured numbers and screenshots on A:.