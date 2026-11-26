---
id: task-25
title: Our own Hard difficulty: stronger enemies through the monster table, behind an option (D-004)
status: Verified
---
## Problem before solution
The game has only Normal and Breezy. The owner wants a harder mode (2026-11-25). Enemy stats live in data tables (`romfs/Data/StreamingAssets/data_tbl/monster.tbl`, MS-NRBF serialized), so a Hard mode can be data, not code.
## Outcome and scope
An option `difficulty_plus = off | hard` (off is faithful). Hard serves the game a modified monster table when it opens it (file overlay in our fs HLE, never touching the dump): HP, attack, defense, magic attack and magic defense scaled by settable factors (defaults about 1.5x HP and 1.25x damage stats, tunable in `config/options.ini`), bosses tunable separately; EXP and coins unchanged unless a setting says so. Works with the game's own Normal setting underneath. Out of scope: new enemy AI, post-game unlock (task-24).
## Dependencies and ownership
Starts after task-24 is Verified (owner 2026-11-25: "1 first"). Needs the table codec (our own MS-NRBF reader and writer, round trip byte-identical). The end-to-end test needs our runtime to reach a battle; until then, test the codec and the overlay on the real file. Owns `tools/datatbl/` (codec, tests), `runtime/mods/datatbl_overlay.*`, a small hook in `runtime/hle/hle_fs.c` for file overlays, the option key `difficulty_plus` and its factors (the overlay, task-16, shows them on a Gameplay page). Decoded tables stay on A: (`A:\AI\Recomps\smrpg-work\data_tbl\`), never in Git.
Execution: Codex worker `/root/hard_difficulty` for implementation. The lead owns integration and verification.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Codec round trip | Every `data_tbl/*.tbl` decodes and re-encodes byte-identical | test output | PASS. 98 of 98 top-level and 126 of 126 including nested tables. |
| AC2 | Scaling correct | With hard on, each monster's listed stats are scaled and clamped to the field's range; with off, the served file is the original bytes | unit test, diff summary | PASS. The focused suite passed 8,154 checks across config parsing, byte equality, stat scaling, clamping, rewards, and failure handling. |
| AC3 | Served in game | Our build logs the overlay serving the modified table when the game opens it, and the game parses it (no exception) | log | PASS. The rebuilt runtime served a table with 249 monsters and 1,164 changed stats and continued loading. The run recorded zero unknown IPC commands. |
| AC4 | Felt in battle | In a run, an enemy's HP in battle matches the scaled value | capture | PASS. Enemy 1001 initialized at 10 HP with Hard off and 15 HP with Hard on. Both values match the in-battle maximum. |
## Human gates
The owner tunes the factors by playing; the defaults are a starting point.
## Recovery and return condition
Return once at done or blocked.
## Approach and commands
Files: extracted romfs under `A:\AI\Recomps\smrpg-work\v101\` (find the `data_tbl` folder there). `docs/analysis/prior-art.md` row 10 names a public tool that round trips these tables (no license: read for facts only, write our own codec, D-009). Build and run commands as in `AGENTS.md` and `docs/PLATFORM.md`.
## Log and handoff
2026-11-25 lead: paused, not rejected. The owner wants the post-game unlock (task-24) first ("just the 1st on", then "1 first"). The first worker was stopped before writing any file; start a fresh worker once task-24 is Verified.

2026-11-25 Codex lead: `/root/hard_difficulty` began format investigation under the `code_worker` role (gpt-6-luna / max). The lead owns the integrated run. Task-26 tracks access to the battle needed for AC4.

The worker implemented the codec, options, and RomFS overlay. The lead ran `python -I tests/mods/run_difficulty_plus_tests.py` and the platform suite after fixing the 256-ID configuration boundary. The platform and options suites passed 392 and 201 checks. Boss factors apply to the IDs listed in `difficulty_plus_boss_ids` in `options.ini`. The source table provides no boss classification, so the list starts empty.

The lead rebuilt with `python -I tools/rtprep/build_game.py --jobs 6`. In the hidden, muted `codex_t25_hard_final` run, the 52,733-byte overlay was served at 42.783 s. The SHA-256 of the source monster table matched the value before both Hard runs. The tested executable SHA-256 is `1b27ebc4c2fb15201f6245b0cdefc8e9f99a12657233554876f3738a09827a8b`. Evidence is in `A:\AI\Recomps\smrpg-work\build\logs\codex_t25_hard_final.log`.

The shared regression run encountered audio edits in progress. The lead copied the Hard changes into a local verification snapshot on A: and reran the project check. The input, lifter, reuse, and runtime suites passed. The sync stress test reported two 60 s queue stalls followed by an access violation. The harness frees the queue after a timeout while worker threads can still access it. The synchronization implementation matches 1b23dda.

On the same snapshot, all 16 sync checks and the crypto vector check passed in standalone reruns. The aggregate receipt records the original sync failure. The receipt and both rerun reports are under `A:\AI\Recomps\smrpg-work\verification\codex-hard-1b23dda\evidence\`.

### Opening battle verification

Both hidden, muted runs loaded an unchanged copy of the codex_t26_02 opening autosave and replayed the same input. The guest moved through the first door, loaded the next hall, and entered the first battle. The private trace build logged current and maximum HP from the initialized battle object. Hard off gave 10/10 HP. Hard on gave 15/15 HP at the default 150 percent HP factor and served the modified 249-monster table.

The battle camera, models, action labels, and HUD rendered in both modes. One Hard transition frame showed colored speckling. See frame05220.png in codex_battle_off_03 and frame05400.png in codex_battle_hard_02 for playable battle views. The logs and script are in the matching run folders under the work directory on A:. Both runs used executable SHA-256 904553a5ed539f3106bd7ee5f1074bb436127a4ecd147e4eccc1eaadf832b4a5.
