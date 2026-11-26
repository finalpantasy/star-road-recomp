---
id: task-31
title: Continue hidden verification from Mario's Pad with live scripted input
status: Verified
---

Task 30 verified the prologue and controller movement at Mario's Pad. Each script change required restarting the game and reloading the save. The Codex lead owns the live scripted input path and the follow-on replay.

| ID | Criterion | Required evidence | Result |
|---|---|---|---|
| AC1 | Apply updated controller commands in a running replay | Live input acceptance, guest pad reads, and resulting movement | PASS. The guest read B after the first update and jumped from the peg. A later update moved Mario through the door. |
| AC2 | Keep the active script when an update is rejected | Rejection log and a guest read from the last accepted script | PASS. The runtime rejected an unknown button. The guest still read the later stick input from the accepted schedule. |
| AC3 | Extend the verified route beyond Mario's room | Captures and logs of the scene and field inputs | PASS. Mario passed Toad's save tutorial, reached the world map, and entered Bowser's Keep. |

Opt in with `SMRPG_INPUT_LIVE_FILE`. Atomically replace a local text file to install a new schedule. Offsets start at the present sampled when the runtime accepts the replacement. Keep the save copies, input schedules, and captures on A:.

## Replay evidence

- `captures/codex_live_route_01` on A: the runtime accepted a two-entry script at present 5405. B released Mario from the peg. The next replacement had an unknown button and was rejected. The guest still read the scheduled southwest stick input at pad call 5482, after the rejection.
- Live updates opened the house door, advanced Toad's save tutorial, and crossed the yard exit. Present 10800 shows the world map. Present 11400 shows Mario approaching Bowser's Keep. The game wrote a checkpoint at the entrance. The 240 s replay logged 4428948 draws through present 11880, zero skips, failed pipelines, Vulkan errors, and zero unknown IPC. Three interpreter fallbacks remained. The executable SHA-256 is `3dcfe6c753c978283d44f889b4cb7331d869f447fc734f2e376a10c2075a348a`.
- Platform checks passed all 392 input and audio checks and 225 options checks. All six project suites passed. Receipt `1795677115575061200-aa7763c9` matched the current inputs and artifacts.
- `captures/codex_live_route_02` on A: reloaded the Keep checkpoint, rendered the bridge collapse, and returned to the world map at Vista Hill. A live directional update moved the selector to Mario's Pad. The game returned to the yard and wrote a Mario's Pad checkpoint after the collapse. The 190 s replay logged 3303110 draws through present 6960 with zero skipped draws, failed pipelines, Vulkan errors, or unknown IPC. Mushroom Way has yet to be reached.
