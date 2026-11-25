---
id: task-19
title: Themed installer
status: Planned
---
## Problem before solution
Owner to-do (2026-11-24): a really nice installer, themed to the game and its characters.
## Outcome and scope
An installer that checks the dump (1.0.0 or 1.0.1), extracts, builds or installs, and creates shortcuts, with the same theme as the launcher. No game files shipped.
## Dependencies and ownership
Starts after our own runtime renders its first frame (owner, 2026-11-24). Needs task-18 theme.
Execution: subchat, Sonnet high; design review by the owner.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Clean install on a fresh Windows user from a dump folder works end to end | Run | Log | NOT_RUN |
## Human gates
The owner approves look and feel (docs/ART_DIRECTION.md); technical PASS does not approve appearance.
## Recovery and return condition
Return once at done or blocked. Runs hidden and muted.
## Approach and commands
Study the baseline recomp's options menu and the game's own settings screens (captures on A:) first. Our own code; D-009 for anything reused.
## Log and handoff
Record measured numbers and screenshots on A:.