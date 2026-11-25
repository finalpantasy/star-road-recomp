---
id: task-20
title: UX polish pass: spacing, layout, usability
status: Planned
---
## Problem before solution
Owner to-do (2026-11-24): professionally made, with even spacing; well planned, well built and well optimized for human use.
## Outcome and scope
A design system (grid, spacing scale, type scale, colors, focus states) used by task-16 to task-19, plus a usability pass: keyboard, controller, mouse, scaling at 1080p, 1440p, 4K and ultrawide.
## Dependencies and ownership
Starts after our own runtime renders its first frame (owner, 2026-11-24). Applies to task-16 to task-19.
Execution: subchat, Sonnet high; design review by the owner.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Spacing and alignment audit passes on every screen at 4 resolutions | Test | Captures | NOT_RUN |
## Human gates
The owner approves look and feel (docs/ART_DIRECTION.md); technical PASS does not approve appearance.
## Recovery and return condition
Return once at done or blocked. Runs hidden and muted.
## Approach and commands
Study the baseline recomp's options menu and the game's own settings screens (captures on A:) first. Our own code; D-009 for anything reused.
## Log and handoff
Record measured numbers and screenshots on A:.