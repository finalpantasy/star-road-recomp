# Post-game boss rematches: what unlocks them (task-24)

Version: Super Mario RPG (Switch) 1.0.1, Unity 2021.3.15f1, IL2CPP. Research done from the owner's own dump, read-only. This note holds flag ids, method names, addresses and my own descriptions only. No game text, table rows, scripts or saves are in it or in Git.

## Bottom line

The rematch gate is four global flags plus one marker that must be clear. The game itself sets them in a fixed order after the ending: ending save, ticket at the Mushroom Kingdom house, a night in the Marrymore hotel suite, then Star Hill. Setting them directly reproduces the state the developers' own "after the suite" jump row builds. Item by item:

| Flag (global, type 0) | Value wanted | Meaning (my words) | Set by | Read by |
|---|---|---|---|---|
| 2312 | 1 | game cleared | the ending scripts (`e19out7`, chapter group 9) | `Menu.get_is_gameclear_enable` (RVA 0x1D08C00) and field scripts |
| 2311 | 0 | marker: the ending was saved but the suite ticket is not handed out yet | the ending scripts (set); the Mushroom Kingdom house script clears it | the house script `f01m1f1` (group 8) |
| 2025 | 1 | suite ticket received | house script `f01m1f1` (group 8) | the Marrymore scripts |
| 2026 | 1 | stayed in the suite | the Marrymore suite event | `d08b1n`, `d10f9b`, `d11out2`, `d11out3`, `d13b5f`, `d15b3a` (all group 8) |
| 2027 | 1 | Star Hill visited after the suite | `d11out2` (group 8, object 22) | Star Hill `d11out3`: every group reads it, and group 8 gates the rematch-related objects |

Not set by the option: flag 2311 stays 0 (it is cleared, not set). Chapter number is not changed unless the owner asks (see "Chapter number" below).

## Evidence

1. Developer jump tables. The game ships its own debug tables under `StreamingAssets/data_tbl/` (`event_status`, `event_flag_status`). `GlobalWork.set_debug_status` clears all flags and then applies rows 1 to N cumulatively. The rows around the ending are a labeled sequence (labels are the developers' own, paraphrased here):
   - row 55: weapon factory after the last story boss
   - row 56: Mario's house after the ending save: flags `[2312]` plus the pending marker `[2311]`
   - row 57: Marrymore village after getting the suite ticket: flag `[2025]`
   - row 58: Star Hill after the suite stay: flag `[2026]`
   - row 59 to 61: Monstro Town with the added bosses beaten one at a time (rematch progress states, set by play, not by the gate)
   The unlock list is exactly the flags rows 56 to 58 add on top of the story flags, minus the marker.
2. Field scripts. The game's compiled scripts (`StreamingAssets/script/*.bin`, 12 groups, group index equals `GlobalWork.chapter_number`) were decoded with a small reader (`tools/postgame/script_scan.py`). Record layout: `[cmd, (false_offset << 8) | nargs, args...]`. Command 3 reads a flag (type, index, expected value), command 4 writes one (type, index, value). Findings:
   - `f01m1f1` (group 8) reads 2311 (expected 1) and 2025 (expected 0): the ticket event runs only in that state.
   - `d11out2` (Star Hill, group 8) reads 2026 and 2027 on init and sets 2027 in object 22.
   - `d11out3` (Star Hill, floor 517) reads 2027 in every group (0 to 8). Group 8 objects 42, 44, 46, 49, 51, 53 and 55 also read the rematch progress flags 2029, 2036, 2042, 2039, 1772, 2033 and (1787 or 2046). The reads use both expected values to pick behavior before and after the rematch. The option never writes these progress flags.
   - Monstro Town's exterior is `m06out` (floor 115). The two in-game checks captured the rematch hosts in `m06m5f1` (floor 123) and `m06m3f1` (floor 119).
   - `e19in3` and `e19out7` (group 9) set 2311, and `e19out7` also sets 2312: the ending save.
3. Code. Flag accessors (all in `GlobalWork`): `set_global_flag` (RVA 0x188B2E0), `get_global_flag` (0x1888BF0), static `set_flag(type, index, flag)` and `get_flag(type, index)` (type 0 global, 1 area, 2 local), `clear_all_flag`. The save: `SaveData._global_flags` is a `BitArray` at +0x1E8; the file stores it as `_global_flag_save`, a `bool[]` at +0x228 (`SaveData.save` RVA 0x18BAD00, `load` 0x18BB3E0, `save_global_flag` 0x18BACA0, `check_global_flag` 0x18BACD0). `_chapter_number` is `int` at SaveData +0x78.
4. Rematch bookkeeping in code: `BattleManager.add_boss_lose_flag_set` (coroutine, RVA 0x1CCD260) and `GlobalWork.auto_save_at_add_boss_lose(int flag_id)` (RVA 0x1889B10) handle the "lost to an added boss" flags. Lose flags seen in scripts: 2028, 2032, 2035, 2038, 2041, 2045, 2049. Dojo and rematch-room flags run 1770 to 1789; 1788 is the all-cleared marker. 2048 and 2300 appear in the scripts but I did not work out their role. None of these gate access, and the option does not touch them.

## What the option does

`unlock_postgame = off | on` (off is faithful). When on, at the one defined point, save load:

- `runtime/hle/hle_fs.c` serves a save file opened for reading (name starts with `SMR_Save`) from memory after `postgame_filter_save` (`runtime/mods/postgame.c`) sets 2312, 2025, 2026, 2027 and clears 2311 in `_global_flag_save`, logging each decision (`postgame: flag 0/<id> <name> <old> -> <new>` or `already <v>`).
- The file on disk is not rewritten by the option. The game's own next save writes the flags (see the owner gate below).
- Off: the option is read once; no save is read or copied, and the code path returns before touching any bytes.
- The save is UTF-16 JSON (`SMR_Save`, written by Unity's `JsonUtility`). The patch edits only the boolean tokens of the flag array (and appends `false` entries if the array is shorter than id 2312); every other character is copied unchanged.
- Option source: `SMRPG_UNLOCK_POSTGAME` (0, 1, off, on) first, else key `unlock_postgame` in section `[gameplay]` of `options.ini` (the file `SMRPG_OPTIONS` names, else beside the exe). The overlay (task-16) shows the same key on its Gameplay page; its own store may read the same file.

## Open items (not verified)

1. Chapter number. The rematch scripts live in script group 8, and `GlobalWork.chapter_number` selects the group. A save below chapter 8 may therefore not show the rematch hosts even with all four flags set. Lifting `_chapter_number` to 8 is a story skip (other areas then run their post-game scripts against mid-game progress). The module can do it (`raise_chapter`), but it is off, and the owner decides. Which chapter value the game's own post-game save holds (8 or 9) is not confirmed: the ending scripts sit in group 9 and the rematch scripts in group 8.
2. Boss identities per rematch host. The object-to-flag table above is from script structure only; I did not match each host to a battle id.
3. A real autosave was copied to build the two Monstro Town fixtures for AC4. A field-by-field round trip test on a real save is still pending for AC3.
4. New game. With no save file there is nothing to patch, so the option has no effect inside a first session. After the first save and reload it applies. An in-memory hook (call `postgame_apply_flags` from a swap of `GlobalWork` new-game setup) needs the stage 2 function swap mechanism, which does not exist yet.

## Owner gate: permanent clearing

Because the option patches the loaded image and the game then saves normally, the flags (including "game cleared", 2312) become permanent in that save at the next in-game save. Turning the option off later does not remove them. The alternative is a write-side strip (remove the flags this option added before the game writes), which keeps saves faithful. Not built; the owner picks.
