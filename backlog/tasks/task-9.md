---
id: task-9
title: Prior art for this game (other recomps, ports, mods, emulator notes), ideas only
status: Ready
---
## Problem before solution
Others may have started a Super Mario RPG (Switch 2023) recompilation, decomp, PC port or mod toolkit. Their findings can save us time. The owner's rule: take ideas, never their code.
## Outcome and scope
`docs/analysis/prior-art.md`: every project found (static recomps, decomps, IL2CPP dumps, mod loaders and mods such as GameBanana entries, randomizers, save editors, asset tools, frame-rate or resolution patches, emulator compatibility notes and game-specific fixes in Ryujinx, yuzu forks, Citron, Eden, Sudachi trackers), each with link, status, license, what it figured out about this game (formats, functions, frame-rate logic, render path, bugs), and which idea we could use. Separately list game-specific emulator bug reports, since they often show engine quirks. Non-goals: downloading, building or running anything; copying code.
## Dependencies and ownership
None. Owns only `docs/analysis/prior-art.md`.
Execution: subchat, Sonnet, medium effort (web research).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Each project has a verified link, status and license | Doc review | Doc | NOT_RUN |
| AC2 | Top ideas mapped to our task cards (task-2 to task-7) | Doc review | Doc | NOT_RUN |
## Human gates
None.
## Recovery and return condition
Doc only. Return at done.
## Approach and commands
Web search, GitHub search (`gh search repos`, `gh search code` read calls), page reads. For sites that block plain fetches (Reddit, GameBanana, Discord-indexed pages) use the `reddit-reader` skill or the built-in browser, read only. No sign-ins, downloads, posts.
## Log and handoff
List sources searched and those that failed.
