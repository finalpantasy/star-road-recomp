# Super Mario RPG PC: static recompilation, then full decompilation
Native PC version of Super Mario RPG (Switch, 2023, v1.0.1) built from the owner's own dump: first an ahead-of-time ARM64 to x86-64 static recompilation, then a full decompilation of the game onto a lightweight native engine.
Original: Unity 2021.3.15f1, IL2CPP, NVN, CRIWARE movies. Target: Windows x64 first, controller plus keyboard. Decomp language: OPEN (D-003).

## Commands and evidence
- Inputs (owner dumps): `F:\Super Mario RPG\*.xci`, `*.nsp`. Never modify, move or commit them.
- Work data (extracted, keys, builds, generated code): `A:\AI\Recomps\smrpg-work\`. Backups: `A:\AI\Recomps\smrpg-backup\`.
- Check: `python tools/game_check.py run --root .` (0 PASS, 1 FAIL, 3 NOT_RUN). Verify freshness: `python tools/game_check.py verify --root .`
- Run: none yet (no runnable build). Capture: none yet.
- Read STATE.md and the task card before work; commands above must be verified, not guessed.
- Verify each acceptance criterion. Tests, rendering, performance and human approval are separate evidence.

## Shared workflow
- Both Claude and Codex read these project rules; the same task/state/evidence applies in either host.
- "The owner" means the project owner (the person running this port). One lead owns integration (the coordinator chat). Use current host delegation/model policy.
- On entry or handoff, read the coordination record in STATE. Reconcile changed policy/tools; do not make the owner repeat setup.
- Continue routine authorized work; ask only for missing consequential choices or required gates.
- Tasks live in `backlog/tasks/`. Only met dependencies make a task Ready. Verified is technical; Accepted adds the owner's decision.
- Keep STATE current and decisions in docs/DECISIONS.md. Preserve unrelated work and rejected directions.
- Repeated failed hypotheses require diagnosis, not blind retries or weakened criteria.
- Log the owner's corrections in docs/CORRECTIONS.md; on the second time, promote to an earned rule below.
- Context checkpoint: every chat (lead and workers) hands off at about 300k tokens (owner limit, overrides the 400k-500k default).

## Earned rules
Binding. Each came from a correction the owner made twice; the why is in docs/CORRECTIONS.md. Do not reword, weaken or remove one without the owner's yes.
None yet.

## Project constraints
- Quality bar (the owner, 2026-11-24): aim for the best recompilation and decompilation ever made. Take ideas from other recomps (New Pokemon Snap, ACNH, LAS, N64Recomp, XenonRecomp, UnleashedRecomp); do not copy their code wholesale. Our own design, written for this game.
- Third-party tools (hactool, nstool, Ghidra, SwitchLoader, IL2CPP dumpers, LLVM) may be used as tools. Code copied into this repo must be license-compatible and credited.
- Nothing from the game ever enters Git: no binaries, assets, keys, metadata, extracted text, generated recompiled code, or dumps. Generated code lives on A: and is rebuilt from the user's own dump.
- Public repo: github.com/finalpantasy/star-road-recomp. Push with `GH_TOKEN=$(gh auth token -u finalpantasy)` and `git -c credential.helper= -c "credential.helper=!gh auth git-credential" push`; never switch the active gh account. Before every push grep the diff and metadata for personal details.
- Public identity: commits as `finalpantasy` with its noreply email. No real names, personal emails or `C:\Users\...` paths in any committed file or message.
- Correctness is measured against the original: every subsystem gets a reference test (recorded inputs, state hashes, captures) before it is called done.
- Upgrades (4K, high frame rate, quality of life, mods) are built from day one (D-004), each behind an option with a faithful mode for A/B checks.
- Heavy processes (lifter runs, full builds, game runs): one at a time, BelowNormal priority, under 10 minutes unless asked, closed when done. Kill only your own PIDs (`taskkill /PID <pid> /T /F`), never by name.
- Big outputs (extraction, generated C, build trees) go on A:, never in this repo.
- No automatic hooks, installs, downloads, external writes, commits or pushes without the owner's yes for that action.
- Never open a visible game window on the owner's desktop; run windowed tests hidden.
- Every background, headless or hidden run is muted (the owner, 2026-11-24): SDL builds set `SDL_AUDIO_DRIVER=dummy` (SDL3) and `SDL_AUDIODRIVER=dummy` (SDL2); our runtime's audout uses a null sink unless `--audio` is passed.
- No em dashes in any file, comment, commit message or user update.
- Follow docs/ART_DIRECTION.md: upgrades must keep the original look; technical PASS does not approve appearance or feel.
