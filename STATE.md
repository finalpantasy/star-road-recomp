# Current state
Milestone/task: M0 setup and analysis (task-0 to task-3, task-7 dispatched 2026-11-24).
Status: Building; technical evidence: baseline check PASS (receipt in evidence/runs, deliberate fail with empty work dir returned FAIL); human gates: D-003 language OPEN, D-005 public repo timing and decomp source publication OPEN.
Last verified source/build and receipt: inputs per tools/inputs.json; extraction on `A:\AI\Recomps\smrpg-work` (base, v101 exefs and romfs, 1,934 files, 6.82 GB).
Dependencies/blockers/open decisions: task-4 waits on task-2 and task-3. Downloads (Il2CppDumper, Cpp2IL, cmake) need the owner's yes.
Integration owner and active file/resource owners: lead = coordinator chat. See table.
Next action and skill: on each worker return, inspect its evidence, update the board and card, then start task-4 (Opus, high) when task-2 and task-3 are Verified.
Resume/run commands: `python tools/game_check.py run --root .`
Git: local repo at `F:Super Mario RPG`, branch main, commits allowed at verified milestones (owner yes 2026-11-24); GitHub: public repo under finalpantasy (owner yes 2026-11-24).

## Coordination
Host and routing source/version: Claude desktop app, Code tab; `~/.claude/policies/model-routing.md` (Sonnet default, Haiku mechanical, Opus with a stated reason, Fable only on opt-in).
Lead role; authorized delegation scope; worker limit: lead orchestrates and checks; the owner asked for one subchat per task (2026-11-24); up to 5 concurrent subchats; one heavy process at a time machine-wide.
Resolved task-to-model/effort routes and opt-ins: task-0 Sonnet medium; task-1 Sonnet low; task-2 Sonnet high; task-3 Sonnet medium; task-7 Sonnet medium; task-4 Opus high (architecture).
Available launch/model-selection/completion controls; limitations: start_session with model and effort, get_session to confirm, completion notes arrive automatically; no polling.
Checkpoint/return rule: workers report once at done or blocked (context: hand off at about 300k tokens, owner limit, lead and subchats alike).

| Task | Owner / stable chat ID | Owned files/resources | Intended / observed model and effort | Claim / last confirming evidence / next event |
|---|---|---|---|---|
| task-0 | smrpg-board `local_41a1f11f-bbab-4b02-ac69-921d17015080` | `tools/board/` | Sonnet medium / observed Sonnet 5.5 medium | Done: board https://claude.ai/artifact/7EVDkCV1KPhW9VRBeZKYTE (db `items`). Look approval pending. |
| task-1 | smrpg-extract `local_8002ca73-d772-4b26-89e1-b15956ec4ab4` | `tools/extract.py` | Sonnet low | Verified; lead rechecked ExeFS hashes. Released. |
| task-2 | smrpg-il2cpp `local_5497e2ca-368e-4637-a2f3-71a0076a47b5` | `tools/il2cpp/`, `docs/analysis/il2cpp.md` | Sonnet high | Running. |
| task-3 | smrpg-survey `local_866547cc-3341-42a9-bdcc-61422d9c1a77` | `tools/survey/`, `docs/analysis/platform-survey.md` | Sonnet medium | Running. |
| task-7 | smrpg-bugs `local_6d272edb-9ebe-457e-88ae-7e61663e7af4` | `docs/analysis/known-bugs-1.0.1.md` | Sonnet medium | Second pass running (browser and reddit-reader) after a thin first pass. |
| task-8 | smrpg-retools `local_3f1d12dc-77cb-41dd-90eb-0d76b6584edf` | `docs/analysis/re-tools.md`, `F:\AI\Recomps\research_notes\ai-re-tools-2026-11.md` | Sonnet medium | Running; scope widened by message 2026-11-24 (skills, frameworks incl. Kirikae, verification, runtime tools, workbench idea). |
| task-9 | smrpg-priorart `local_56d775aa-ae3d-4427-8281-1f1c2da48f99` | `docs/analysis/prior-art.md` | Sonnet medium | Running. |
