---
id: task-25
title: Our own Hard difficulty: stronger enemies through the monster table, behind an option (D-004)
status: Ready
---
## Problem before solution
The game has only Normal and Breezy. The owner wants a harder mode (2026-11-25). Enemy stats live in data tables (`romfs/Data/StreamingAssets/data_tbl/monster.tbl`, MS-NRBF serialized), so a Hard mode can be data, not code.
## Outcome and scope
An option `difficulty_plus = off | hard` (off is faithful). Hard serves the game a modified monster table when it opens it (file overlay in our fs HLE, never touching the dump): HP, attack, defense, magic attack and magic defense scaled by settable factors (defaults about 1.5x HP and 1.25x damage stats, tunable in `config/options.ini`), bosses tunable separately; EXP and coins unchanged unless a setting says so. Works with the game's own Normal setting underneath. Out of scope: new enemy AI, post-game unlock (task-24).
## Dependencies and ownership
Starts after task-24 is Verified (owner 2026-11-25: "1 first"). Needs the table codec (our own MS-NRBF reader and writer, round trip byte-identical). The end-to-end test needs our runtime to reach a battle; until then, test the codec and the overlay on the real file. Owns `tools/datatbl/` (codec, tests), `runtime/mods/datatbl_overlay.*`, a small hook in `runtime/hle/hle_fs.c` for file overlays, the option key `difficulty_plus` and its factors (the overlay, task-16, shows them on a Gameplay page). Decoded tables stay on A: (`A:\AI\Recomps\smrpg-work\data_tbl\`), never in Git.
Execution: subchat smrpg-hard, Sonnet high.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Codec round trip | Every `data_tbl/*.tbl` decodes and re-encodes byte-identical | test output | |
| AC2 | Scaling correct | With hard on, each monster's listed stats are scaled and clamped to the field's range; with off, the served file is the original bytes | unit test, diff summary | |
| AC3 | Served in game | Our build logs the overlay serving the modified table when the game opens it, and the game parses it (no exception) | log | |
| AC4 | Felt in battle | In a run, an enemy's HP in battle matches the scaled value | capture | NOT_RUN until a battle runs |
## Human gates
The owner tunes the factors by playing; the defaults are a starting point.
## Recovery and return condition
Return once at done or blocked.
## Approach and commands
Files: extracted romfs under `A:\AI\Recomps\smrpg-work\v101\` (find the `data_tbl` folder there). `docs/analysis/prior-art.md` row 10 names a public tool that round trips these tables (no license: read for facts only, write our own codec, D-009). Build and run commands as in `AGENTS.md` and `docs/PLATFORM.md`.
## Log and handoff
2026-11-25 lead: paused, not rejected. The owner wants the post-game unlock (task-24) first ("just the 1st on", then "1 first"). The first worker was stopped before writing any file; start a fresh worker once task-24 is Verified.
