---
id: task-32
title: Verify the route from Mario's Pad into Mushroom Way
status: Verified
---

Task 31 left a post-collapse Mario's Pad autosave. Use a copy of that save, the original v1.0.1 guest code, and live controller inputs to continue the route. The Codex lead owns the replay and capture evidence.

| ID | Criterion | Required evidence | Result |
|---|---|---|---|
| AC1 | Finish Toad's introduction and unlock Mushroom Way | Captures of the guest events and world map selection | PASS. The map selector highlights Mushroom Way at present 8460. |
| AC2 | Enter Mushroom Way and move under player control | Field captures, guest input reads, and an area checkpoint | PASS. The area title appears at 9000, the guest reads movement, and the game writes a Mushroom Way autosave. |
| AC3 | Exercise a Mushroom Way battle | Captures of battle entry, player action, and return to the field | PASS. Two battles show victory and return to controllable field scenes. |
| AC4 | Check runtime diagnostics | Replay exit and IPC, Vulkan, and renderer logs | PASS. Both runs reached their watchdogs with zero skipped draws, pipeline failures, Vulkan errors, or unknown IPC. |

Run hidden and muted under the shared heavy lock. Keep the saves, input files, and captures on A:. A same-version Switch reference is required for visual parity.

## Replay evidence

- `captures/codex_mushroom_way_01` on A: the post-collapse save loaded in the yard. Mario reached the house, spoke to Toad, and triggered the outdoor item and Action Commands tutorials. The 350 s watchdog ended the replay during the Goomba demonstration. The game wrote a later Mario's Pad autosave, which supplies the next replay. The run logged 6657210 draws through present 16680 with zero skipped draws, failed pipelines, Vulkan errors, or unknown IPC.
- `captures/codex_mushroom_way_02` on A: the later autosave loaded at the porch. Mario took the shorter branch when asked about Action Commands and left for Mushroom Way. The first Goomba fight shows victory at present 10260 and returns to the field at 10800. A second fight shows victory at 13770. At 15030 Mario is farther along the path. The guest wrote an autosave at `d02out`.
- The second replay recorded 6017254 draws through present 15000. The existing unused fragment output warning recurred. One interpreter fallback ran 852 times. The run used executable SHA-256 `3dcfe6c753c978283d44f889b4cb7331d869f447fc734f2e376a10c2075a348a`. Project receipt `1795677115575061200-aa7763c9` still matches the runtime inputs and recorded artifacts.
