---
id: task-18
title: Themed launcher
status: Planned
---
## Problem before solution
Owner to-do (2026-11-24): a really nice launcher, themed to the game and its characters.
## Outcome and scope
A launcher that finds the dump, checks it, sets options before start, shows mods and updates. Themed to Super Mario RPG and its characters without using Nintendo assets in our repo (assets loaded from the owner's dump at run time).
## Dependencies and ownership
Starts after our own runtime renders its first frame (owner, 2026-11-24). Needs a stable build entry point.
Execution: subchat, Sonnet high; design review by the owner.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Launch, settings, dump check work with mouse, keyboard and controller | Run | Captures | NOT_RUN |
## Human gates
The owner approves look and feel (docs/ART_DIRECTION.md); technical PASS does not approve appearance.
## Recovery and return condition
Return once at done or blocked. Runs hidden and muted.
## Approach and commands
Study the baseline recomp's options menu and the game's own settings screens (captures on A:) first. Our own code; D-009 for anything reused.
## Log and handoff
Record measured numbers and screenshots on A:.