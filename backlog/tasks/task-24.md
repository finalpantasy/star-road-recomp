---
id: task-24
title: Option: unlock post-game content (boss rematches) without playing through (D-004)
status: Review
---
## Problem before solution
The owner wants the hardest content without playing the whole game first (2026-11-25, "give an option to unlock that without having to play through first"). The game has only two difficulties (`GlobalWork.DIFFICULTY`: NORMAL, ENJOY = Breezy); the hard content is the post-game boss rematches, gated by the game-clear state (`GlobalWork.is_gameclear_enable`, `ExBossBarriorKind`, rematch flags) and by story progress.
## Outcome and scope
An option `unlock_postgame = off | on` (off is faithful). On: the runtime sets the game-clear state and every flag each rematch needs, so the rematches are available, on a new or loaded save, without breaking story progression. Each rematch's flag set is documented. Out of scope: our own Hard mode (task-25), save editing tools beyond what this needs.
## Dependencies and ownership
Static research needs nothing. The end-to-end test needs our runtime to reach gameplay (new game and a loaded save); until then, test against the save format and the lifted code. Owns `runtime/mods/postgame.*`, `docs/analysis/postgame-flags.md`, the option key `unlock_postgame` (the overlay, task-16, shows it on a Gameplay page). Saves under `A:\AI\Recomps\smrpg-work\save\` only, always on copies.
Execution: subchat smrpg-postgame, Sonnet high.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Flags found | Every post-game rematch and what unlocks it (flag ids, where they are read in the lifted code, the IL2CPP method names) | `docs/analysis/postgame-flags.md` | PARTIAL: gate flags 2312, 2311 (clear), 2025, 2026, 2027 found with the debug jump rows 56 to 58, field scripts, and IL2CPP methods and RVAs. Rematch host to boss mapping and the chapter value are open (doc, open items) |
| AC2 | Set safely | The option sets exactly those flags, at a defined point (save load or new game), and logs each one; `off` changes nothing | unit test, log | PASS. The synthetic suite passed 348 checks. Both Monstro Town loads logged the four gate flags changing from 0 to 1 and flag 2311 remaining 0. |
| AC3 | Save round trip | A save written with the option on still loads and keeps its other data (compared field by field) | test output | PASS on synthetic UTF-16 saves with varied formatting, BOM, flag-array length, and chapter position. Other fields matched character for character and a second pass made no edits. LIVE PASS on real saves (2026-11-26): the game loaded a copy of its own opening autosave and wrote a new save in two hidden, muted runs of the same scripted input, option off and on. Off: no flag-array change at all. On: exactly flags 2312, 2025, 2026, 2027 changed 0 to 1, 2311 stayed 0, chapter and array length (4096) unchanged. Every other difference between the two outputs is game play or save bookkeeping (play time, save date and seconds, coins, experience, hp, one defeat count, one area flag, respawn rotation, thumbnail), run to run timing noise, none from the option. Offline path also passes on five other game-written saves (`tests/mods/run_postgame_realsave.py`). Details in the log. |
| AC4 | Rematch reachable | In a run of our build, a rematch is available on a save that has not cleared the game | capture | PASS. Two pre-clear test saves reached Monstro Town. One replay entered battle and the second rendered the rematch intro. |
## Human gates
The owner decides whether unlocking should also mark the game as cleared in the save permanently.
## Recovery and return condition
Return once at done or blocked.
## Approach and commands
Sources: IL2CPP dump `A:\AI\Recomps\smrpg-work\analysis\il2cpp\crosscheck\dumper\dump.cs` (`GlobalWork`), `docs/analysis/il2cpp.md`, saves (`SMR_Save` JSON in UTF-16), `docs/analysis/known-bugs-1.0.1.md` (rematch notes). Public mod tools and wikis may be read for facts, never copied (D-009).
## Log and handoff
- Files: `runtime/mods/postgame.{h,c}`, `tests/mods/test_postgame.c` and `run_postgame_tests.py`, `docs/analysis/postgame-flags.md`, research helpers `tools/postgame/nrbf_read.py` and `script_scan.py` (read-only, output stays on A:). Additive edits: `runtime/hle/hle_fs.c` (HostFile gets `mem`/`mem_n`; `file_read`, `file_get_size`, `fsys_open_file` serve a patched save image; helper `save_unlock_view`), `runtime/CMakeLists.txt` (one `list(APPEND SMRPG_RT_SOURCES mods/postgame.c)` line).
- Option read: plain key read (`SMRPG_UNLOCK_POSTGAME`, else `[gameplay] unlock_postgame` in `SMRPG_OPTIONS` or options.ini beside the exe). I did not add the key to `config/options.default.ini` or the overlay table: `tests/platform/test_options.c` compares that file with the table's emitted text, so the overlay worker must add `unlock_postgame` (section `gameplay`, off/on, default off) to its table and regenerate the ini. My reader needs nothing else.
- Not run: a full `build_game.py` build (it replaces the shared `smrpg.exe` in the work build folder other workers use). `hle_fs.c` passes `clang -fsyntax-only`; the lead should build and watch for `hle fs: save ... served with the post-game flags set` with `SMRPG_UNLOCK_POSTGAME=1`.
- Owner gates: (1) permanent clearing: the flags land in the next in-game save and stay; a write-side strip is not built. (2) lifting `_chapter_number` to 8 is a story skip and is off (`raise_chapter` in `postgame_patch_save`); a save below chapter 8 may not show the rematch hosts without it.
- Open: boss identity per rematch host, whether the game's own post-game save holds chapter 8 or 9, new-game effect (needs a save reload; in-memory hook waits for stage 2 swaps), 2048 and 2300 roles.
2026-11-25 owner: keep the unlock permanent (flags stay in the save after the next in-game save, even with the option off later).
2026-11-25 owner: chapter is left untouched (lead recommendation). Revisit with a real save: if the rematch hosts need chapter 8, propose a separate off-by-default option.

### AC4 replay

The two hidden, muted runs loaded disposable copies of the task-26 autosave. The fixtures use chapter 8, preserve a clear flag of 0 before startup, and carry the progression required for their floors. With unlock_postgame enabled, the load log records the four gate changes and marker 2311 already clear. The runtime option leaves the fixture chapter alone. The two hosts rendered in the private captures. One replay entered the playable fight. The other reached the rematch introduction. Both runs reported zero unknown IPC commands.

Capture and log references are kept with the private replay files on A:. Both runs used executable SHA-256 8b4d7dabfcb489b2874ec5e4371669a40b065bf4f999502abbcdb453902c98e8.

2026-11-26 AC3 real save: copies of the opening autosave under `A:\AI\Recomps\smrpg-work/task24_rt/` (o_off, o_on), input of codex_opening_route_02, 200 s each. Off run wrote 201472 to 382174 bytes, 0 flag-array diffs vs the loaded save. On run wrote 382170 bytes with 2312, 2025, 2026, 2027 set and 2311 clear; log shows the four 0 to 1 decisions at each load. On-vs-off outputs differ only in the four gate flags plus play noise (play time, save date and seconds, coin, experience, hp, one defeat count, one area flag, respawn rotation, thumbnail bytes). The flags persist in the game's own write (owner gate: permanent unlock, as decided). Reusable offline test: `python -I tests/mods/run_postgame_realsave.py SAVE_FILE` (reads the save, patches a copy through `postgame_filter_save`, asserts off returns NULL, only the gate flags differ, a second pass is a no-op); passed on six real saves, including mid-game slots (chapter 2, array 4096). A second route that loaded a mid-game save for 240 s did not reach a save write.
