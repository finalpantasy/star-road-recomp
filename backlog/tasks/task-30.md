---
id: task-30
title: Verify the opening route through the first boss
status: Verified
---

## Outcome

Use the original v1.0.1 executable through the runtime. Start from a disposable copy of the opening autosave, reach the first boss, and confirm the return to a controllable field after the fight.

## Dependencies and ownership

The Codex lead owns the scripted input and capture evidence. Tasks 12, 21, 22, 25, and 26 supply the renderer, movies, controller, and starting save.

| ID | Criterion | Evidence | Result |
|---|---|---|---|
| AC1 | Pass the opening encounters and proceed past their room | Captures on both sides of the room transition and scripted controller reads | PASS. The adjusted route opened the door and crossed the lava bridge. |
| AC2 | Enter and finish the first boss fight | Battle render, recorded inputs, and an observed victory or ensuing guest scene | PASS. The throne-room reload targeted the chandelier and reached the cutscene after the fight. |
| AC3 | Restore player control after the boss | Captures before and after directional input and the corresponding guest read | PASS. The Mario's Pad autosave loaded. B cleared the Jump prompt, and stick input moved Mario around the room. |
| AC4 | Verify runtime behavior along the route | Process exit, IPC and renderer diagnostics, and capture evidence | PASS. The replays stayed live until their configured watchdogs. Logs showed zero Vulkan errors, failed pipelines, skipped draws, or unknown IPC. |

Keep captures and save copies on A:. The runner uses the shared heavy slot and stays hidden and muted. Switch visual parity requires a separate same-version reference.

The replays used the fixed HDR-copy executable with SHA-256 `6ae69b38ded4f98bad0da317be66da0896e922b659c4ee8fe7cf7db056523cb5`. Each capture directory contains its input script, Vulkan diagnostics, and disposable save copy. The executable runs with the default options, Vulkan validation, both SDL audio drivers set to `dummy`, and the window hidden.

## Replay evidence

- `captures/codex_opening_route_01` on A: cleared the first two encounters and wrote an autosave for the next room. The recorded stick input left Mario beside the exit in that room. The final capture shows where a southeast correction is needed before approaching the doorway again. The replay recorded 3910069 draws through present 17520 with zero skips or pipeline failures. All 102 IPC commands were recognized. One interpreter target ran 9650 times.
- `captures/codex_opening_route_02` on A: added a southeast correction followed by a northeast approach. Presents 7400 and 7550 show Mario at the exit and the open doorway. The lava bridge appears at present 7700, and the throne room at 8500. The chandelier battle menu appears at 9500. Pressing Attack selected Bowser and did not finish the fight. The replay recorded 3291349 draws through present 10920, zero skips, failed pipelines, Vulkan errors, or unknown IPC, and two save commits. One interpreter target ran 1898 times.
- `captures/codex_opening_route_03` on A: reloaded the bridge checkpoint from route 02 and started in the throne-room approach. The script pressed Left during target selection. Present 10400 shows Chandelier selected. At 11400 the battle HUD has disappeared and the chandelier cutscene has begun. The 235 s watchdog ended the process during that cutscene. The replay recorded 4843564 draws through present 13080 with zero skips or failed pipelines and zero unknown IPC. One interpreter target ran 1604 times. The game wrote a throne-room autosave, which supplies the input for route 04.
- `captures/codex_opening_route_04` on A: repeated the boss clear and ran through the two post-boss VP9 movies, which decoded 2440 and 186 pictures with zero decode or texture errors. At 323.715 s the filesystem committed a Mario's Pad autosave. The 335 s watchdog ended the process after the next scene began drawing. The capture window ended during the movies. Through present 34920, the replay logged 5757549 draws with zero skips, failed pipelines, Vulkan errors, or unknown IPC. The known unused color output warning recurred. Two interpreter targets ran 301 and 15 times.
- `captures/codex_opening_route_05` on A: the new autosave loaded into Mario's room. The Jump prompt remained visible while Mario hung on the wall peg. The route had only A and stick input. Route 06 adds B before checking movement.
- `captures/codex_opening_route_06` on A: the guest read B immediately after present 5500. Present 5550 shows Mario on the floor. The guest then read a northeast stick input, and present 5650 shows Mario at the east wall. At 5950 he is on the opposite side of the room. The 102 s replay logged 1789217 draws through present 7320 with zero skips, failed pipelines, Vulkan errors, or unknown IPC. The unused color output warning recurred. Three interpreter targets ran 224, 14, and 19 times.
- The project check receipt `1795675261518811200-0b8bded2` still matches the runtime source and its recorded artifacts. This task added replay evidence and changed only the task documentation and project state.
