---
id: task-26
title: Reach gameplay from the title screen with scripted controller input
status: Verified
---
## Observed behavior

The 1b23dda build displays the title screen in the hidden lead15 run. The handoff reports that scripted attempts stopped at the title. Task-22 proved the guest can read scripted pad state earlier in boot. The first unconfirmed step is guest input during the title sequence.

## Outcome and scope

Start a new game from a fresh save directory and reach the first scene where the player controls Mario. Use the existing guest code and trace input delivery, guest state transitions, and runtime errors.

## Dependencies and ownership

The lead owns integration and the single build and runtime test slot. Depends on task-21 for the title movies and task-22 for controller input. Gameplay evidence unblocks the in-game checks for tasks 24 and 25 and gives task-16 a route to the guest Settings menu.

## Acceptance and evidence

| ID | Criterion | Check | Evidence | Result |
|---|---|---|---|---|
| AC1 | Title input reaches the guest | Confirm a press and release after the title appears | Input script, title capture, and guest input log | PASS. The guest received A and release several times. Present 1680 shows the difficulty menu. |
| AC2 | A new game reaches gameplay | Start with an empty save directory, choose a difficulty, and advance to a scene under player control | Capture before and after a directional input, with a log or state read showing the input took effect | PASS. In `codex_t26_02`, present 9656 shows Mario on the starting rug and present 10308 shows him moving beside the door. The guest read stick values (23170, 23170). |
| AC3 | Regression check passes | Validate the tested source with the relevant platform tests and project check | Test output and fresh receipt | PASS. Six baseline suites passed and the receipt verified. Platform tests passed 392 checks and options tests passed 144 checks. |

## Reproduction

Run `python -I tools/rtprep/run_game.py` hidden with both SDL audio driver variables set to `dummy`. Use a unique log name and set `SMRPG_SAVE` to a disposable directory on A:. Schedule short A presses across the observed title window using `SMRPG_INPUT`. Record the exact inputs and the present numbers where the title and difficulty selection appear. If the title does not advance, compare the presented frame, host pad publication, and the guest's pad read before changing input handling.

The baseline at 1b23dda passed `game_check run` and `verify` under receipt `1795626986474729600-46de807b`. Run `codex_t26_01` used 32-frame A holds every 170 presents from present 1100 and an empty save directory.

The first run entered Normal difficulty, played the opening movie, and rendered Mario inside Bowser's Keep at present 9240. An autosave committed at 125.744 s before the watchdog ended the run. The log records 100 distinct IPC commands, zero unknown commands, and three distinct interpreter fallback targets.

The second run started with an empty save, committed an autosave at 119.075 s, and confirmed movement after the stick hold at present 9660. It ran hidden and muted to its 175 s watchdog with zero unknown IPC commands. The route used the existing runtime.

Scripts and captures are in `A:\AI\Recomps\smrpg-work\captures\codex_t26_01\` and `codex_t26_02\`. Logs are in `A:\AI\Recomps\smrpg-work\build\logs\`. The tested executable SHA-256 is `9f1c7fa866946491963974bac5688d418953fef216c8604d3f9a7463900789d1`.
