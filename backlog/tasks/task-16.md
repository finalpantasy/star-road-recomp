---
id: task-16
title: In-game overlay menu inside the game settings menu
status: Planned
---
## Problem before solution
Owner to-do (2026-11-24): an in-game overlay menu integrated into the actual game settings menu.
## Outcome and scope
Our options (resolution, frame rate, upgrades, QoL, controls) appear as extra pages inside the game's own Settings menu, in its style, plus a hotkey overlay for anything the game menu cannot host. Controller and keyboard.
## Dependencies and ownership
Starts after our own runtime renders its first frame (owner, 2026-11-24; met 2026-11-25). Needs task-12 (NVN), task-13 (HLE), task-22 (SDL3 window and input). Drawn with Dear ImGui (MIT, owner yes 2026-11-25, D-012), fetched at a pinned release into `A:\AI\Recomps\thirdparty\imgui\` by a committed script, never committed itself; styled to match the game's menus.
Execution: subchat, Sonnet high; design review by the owner.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Every option reachable from the game Settings menu with controller only | Run | Capture | NOT_RUN |
## Human gates
The owner approves look and feel (docs/ART_DIRECTION.md); technical PASS does not approve appearance.
## Recovery and return condition
Return once at done or blocked. Runs hidden and muted.
## Approach and commands
Study the baseline recomp's options menu and the game's own settings screens (captures on A:) first. Our own code; D-009 for anything reused.
## Log and handoff
Record measured numbers and screenshots on A:.