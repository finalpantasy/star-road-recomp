---
id: task-24
title: Option: unlock post-game content (boss rematches) without playing through (D-004)
status: Ready
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
| AC1 | Flags found | Every post-game rematch and what unlocks it (flag ids, where they are read in the lifted code, the IL2CPP method names) | `docs/analysis/postgame-flags.md` | |
| AC2 | Set safely | The option sets exactly those flags, at a defined point (save load or new game), and logs each one; `off` changes nothing | unit test, log | |
| AC3 | Save round trip | A save written with the option on still loads and keeps its other data (compared field by field) | test output | |
| AC4 | Rematch reachable | In a run of our build, a rematch is available on a save that has not cleared the game | capture | NOT_RUN until gameplay runs |
## Human gates
The owner decides whether unlocking should also mark the game as cleared in the save permanently.
## Recovery and return condition
Return once at done or blocked.
## Approach and commands
Sources: IL2CPP dump `A:\AI\Recomps\smrpg-work\analysis\il2cpp\crosscheck\dumper\dump.cs` (`GlobalWork`), `docs/analysis/il2cpp.md`, saves (`SMR_Save` JSON in UTF-16), `docs/analysis/known-bugs-1.0.1.md` (rematch notes). Public mod tools and wikis may be read for facts, never copied (D-009).
## Log and handoff
