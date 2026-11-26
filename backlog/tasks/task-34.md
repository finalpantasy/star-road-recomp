---
id: task-34
title: Verify the Chancellor sequence and Bandit's Way unlock
status: Verified
---

Start from the castle entrance autosave from task 33. The Codex lead owns the controller replay, disposable saves, and capture evidence.

| ID | Criterion | Required evidence | Result |
|---|---|---|---|
| AC1 | Complete the first Chancellor audience | Castle reload, throne room captures, and return to town | PASS on the archived replay build. Run 07 spoke directly to the Chancellor, finished the farewell, and wrote a town autosave. |
| AC2 | Recruit Mallow | Story event and two-character party display or guest save | PASS on the archived replay build. Run 08 shows recruitment at present 12150 and both characters in the HUD at 13680. |
| AC3 | Enter Bandit's Way | World-map and field captures plus a guest checkpoint | PASS on the archived replay build. Run 10 selected Bandit's Way at present 4800, entered the field at 6120, and wrote the `d03out` autosave with party order `[1, 2, 0, 0, 0]`. |
| AC4 | Check runtime diagnostics | Replay exit and IPC, Vulkan, and renderer logs | PASS on the archived replay build through Bandit's Way. The separate shared executable launch failed at startup. |

Run hidden and muted under the shared heavy lock. Use disposable saves on A:. Same-version Switch visual parity remains a separate check.

## Replay evidence

- The shared executable (`3dead825c47ff83075672d2eff4ff4b68e36a8d9d7dd24b77648108517f014a6`) lost the GPU device during run 03. Vulkan validation caught a 1920-pixel-wide upload targeting a 1024-wide BC1 texture at startup. Task 12 records region guards exercised on an isolated build.
- The archived executable in `codex_live_route_01` has SHA-256 `3dcfe6c753c978283d44f889b4cb7331d869f447fc734f2e376a10c2075a348a`. Run 04 started from a disposable copy of run 01's throne-room checkpoint. The logs confirm a hidden window, null audio sink, and Vulkan validation enabled. Evidence is in `captures/codex_castle_route_04` on A:.
- At the 520-second cutoff, run 04 had logged 102 distinct IPC commands with zero unknown, zero Vulkan errors, and 8,967,631 draws through present 22440 with zero skips and zero pipeline failures. The validator logged the recurring unused fragment output warning. The guest made one distinct interpreter fallback.
- Toad's introduction ends at the foot of the throne-room stairs. Runs 04 to 06 left the audience incomplete. In run 07, Mario climbed the stairs, spoke with the Chancellor at present 4080, declined the equipment tutorial, and completed the farewell. On returning to town, the guest wrote an `m01out` autosave and played the Croco and Mallow movie. The decoder logged 3,476 pictures and zero decode or texture failures. Present 32100 shows Mario next to Mallow outside the inn. The 420-second cutoff came before recruitment.
- From the new `m01out` autosave, run 08 spoke to Mallow outside the inn, responded to the jump prompt, and accepted the request. The resumed field HUD shows Mario at 25 HP and Mallow at 20 HP. Run 08 reached the 335-second cutoff with zero unknown IPC, Vulkan errors, skipped draws or pipeline failures. The autosave still records the state before recruitment.
- Using the accepted inputs from run 08 as a startup script, run 09 recruited Mallow again, completed the encounter at the east bridge, received the map, and exited town. The guest wrote a `wld00` autosave with two party members. Reloading it in run 10 started Mario on the bridge. From the world map, D-pad Down selected Bandit's Way and A entered the field. The game wrote `d03out`, and present 8760 shows field controls with both party members. Evidence is in `captures/codex_castle_route_10` on A:.
- A stick input at present 9649 moved Mario farther along the path by 10200. At the 190-second cutoff, the game had logged 102 distinct IPC commands, zero unknown, and 2,887,056 draws through present 12240. No draws were skipped, all 132 pipelines succeeded, and Vulkan validation recorded zero errors.
