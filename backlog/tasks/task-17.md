---
id: task-17
title: Live reload of settings, no restart
status: Planned
---
## Problem before solution
Owner to-do (2026-11-24): as many settings as possible work without a restart (live reload or hot swap).
## Outcome and scope
Each option is classed live, next-scene or restart. Target: everything live except what truly needs a device or swapchain rebuild, and those rebuild in place without quitting.
## Dependencies and ownership
Starts after our own runtime renders its first frame (owner, 2026-11-24). Needs task-16.
Execution: subchat, Sonnet high; design review by the owner.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | List of every option with its class; restart-only count is minimal and each has a reason | Doc | Table | NOT_RUN |
## Human gates
The owner approves look and feel (docs/ART_DIRECTION.md); technical PASS does not approve appearance.
## Recovery and return condition
Return once at done or blocked. Runs hidden and muted.
## Approach and commands
Study the baseline recomp's options menu and the game's own settings screens (captures on A:) first. Our own code; D-009 for anything reused.
## Log and handoff
Record measured numbers and screenshots on A:.