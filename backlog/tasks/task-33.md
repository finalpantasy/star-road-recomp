---
id: task-33
title: Verify the rest of Mushroom Way and entry to Mushroom Kingdom
status: Verified
---

Start from the Mushroom Way autosave verified in task 32. The Codex lead owns the controller replay and evidence. Keep the private saves, input schedules, and captures on A:.

| ID | Criterion | Required evidence | Result |
|---|---|---|---|
| AC1 | Progress through the remaining Mushroom Way fields | Captures of area transitions and controller reads | PASS. The replay crossed the first trail and palm field and reached the Lakitu trail. |
| AC2 | Complete the Hammer Bros. encounter | Battle capture, victory or ensuing story event, and return to the field | PASS. The replay shows victory and the Hammer reward sequence in the field. |
| AC3 | Enter Mushroom Kingdom | Area capture and guest-written checkpoint | PASS. Run 03 shows the arrival bridge and writes a Mushroom Kingdom autosave. |
| AC4 | Check runtime diagnostics | Replay exit and IPC, Vulkan, and renderer logs | PASS. All three runs reached their watchdogs with zero skipped draws, pipeline failures, Vulkan errors, or unknown IPC. |

Use `run_game.py` under the shared lock with Vulkan validation and the window and audio off. Switch visual parity requires a separate same-version reference.

## Replay evidence

- `captures/codex_mushroom_kingdom_01` on A: the replay loaded the Mushroom Way checkpoint and crossed the first two fields. It used a Mushroom during a two-Goomba fight and showed a timed-hit effect at present 18360. The transition at 21240 shows an intact background under the stripes in the palm field. Present 30000 shows the Lakitu trail.
- Mario reached level 2 at present 34080. Reloading the `d02out3` checkpoint starts before that level-up. Run 01 recorded 7828227 draws through present 35400.
- `captures/codex_mushroom_kingdom_02` on A: Mario reloaded the final field, cleared one Spikey fight, selected a level-up bonus, and ran away from the next encounter. During the boss fight, he used Jump, restored HP with a Mushroom, and recovered FP with Honey Syrup. Valor Up appears at present 23500. Victory appears at 30200. Mario raises the Hammer at 33400.
- At the 395 s watchdog, run 02 had logged 7248012 draws through present 33360 and recognized all 99 IPC commands. One interpreter fallback ran 23732 times. Both runs used executable SHA-256 `3dcfe6c753c978283d44f889b4cb7331d869f447fc734f2e376a10c2075a348a`. Project receipt `1795677115575061200-aa7763c9` still matches the runtime inputs and artifacts.
- `input-recorded.txt` in the second run folder reconstructs 286 entries through present 33360 from the 21 accepted live updates. It truncates older entries when a replacement took effect. Run 03 used it as the startup sequence.
- `captures/codex_mushroom_kingdom_03` on A: the recorded startup inputs reached the Hammer Bros. dialogue earlier than in run 02. Live input took over there, cleared the fight again, and advanced past the reward. Mario returned to player control at 32400, reached the world map at 33600, and entered Mushroom Kingdom. The save identified `m01out` on arrival. The replay continued through the castle gate and into the entrance hall at 37950.
- Run 03 ended at the 510 s watchdog. It recorded 9596430 draws through present 39000, with 233 pipelines and zero failures. All 102 IPC commands were recognized. The existing unused fragment output warning recurred, and one interpreter fallback ran 24168 times. The final Mushroom Kingdom autosave is inside the castle at `m01c1f1a`. Project receipt `1795677115575061200-aa7763c9` still verified.
