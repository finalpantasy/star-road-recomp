# Current state
Milestone/task: M1 lifter (task-5, finishing) plus M3 runtime tracks in parallel: task-10 shaders, task-11 textures, task-12 NVN on Vulkan. task-13 HLE services and task-14 runtime hard parts open when task-5 is Verified. Target: first frame in about a day. D-009: reuse first (gitignored reuse/, REUSE_LEDGER.md, guard tests/test_no_reuse.py), then rewrite; nothing reused is ever pushed.
Status: Building; technical evidence: baseline check PASS (receipt in evidence/runs, deliberate fail with empty work dir returned FAIL); human gates: D-003 language OPEN; D-007 decomp timing decided (recomp first, overlap); D-005 public repo timing and decomp source publication OPEN.
Last verified source/build and receipt: inputs per tools/inputs.json; extraction on `A:\AI\Recomps\smrpg-work` (base, v101 exefs and romfs, 1,934 files, 6.82 GB).
Dependencies/blockers/open decisions: task-5 Ready; task-6 waits on task-5. RECOMPILER.md open items: L4 reference original (owner), movie decoder licenses. Tools in `A:\AI\Recomps\tools-ext` (owner approved 2026-11-24): Il2CppDumper 6.7.46, Cpp2IL 2022.1.0-pre21, CMake 4.5.1; digests checked. Prior art: a public MIT recomp of 1.0.0 exists (ElKiro1311/smrpg-recompiled, LASRecomp base), ideas only; our edge is 1.0.1, reference tests against the original, and the decomp.
Integration owner and active file/resource owners: lead = coordinator chat. See table.
Next action and skill: task-13 HLE services and task-14 runtime hard parts running; wire NVN into the build; take task-10 shader cache; integrate toward first frame.
Resume/run commands: `python tools/game_check.py run --root .`
Git: local repo at `F:\Super Mario RPG`, branch main, commits allowed at verified milestones (owner yes 2026-11-24); GitHub: public repo under finalpantasy (owner yes 2026-11-24).

## Coordination
Host and routing source/version: Claude desktop app, Code tab; `~/.claude/policies/model-routing.md` (Sonnet default, Haiku mechanical, Opus with a stated reason, Fable only on opt-in).
Lead role; authorized delegation scope; worker limit: lead orchestrates and checks; the owner asked for one subchat per task (2026-11-24); up to 5 concurrent subchats; one heavy process at a time machine-wide.
Resolved task-to-model/effort routes and opt-ins: task-0 Sonnet medium; task-1 Sonnet low; task-2 Sonnet high; task-3 Sonnet medium; task-7 Sonnet medium; task-4 Opus high (architecture).
Available launch/model-selection/completion controls; limitations: start_session with model and effort, get_session to confirm, completion notes arrive automatically; no polling.
Checkpoint/return rule: workers report once at done or blocked (context: hand off at about 300k tokens, owner limit, lead and subchats alike).
Board rule (owner, 2026-11-24): the work board carries every sub-step, roadmap item, bug, upgrade idea and owner decision as its own card, not one card per task. Add cards when work is split.

| Task | Owner / stable chat ID | Owned files/resources | Intended / observed model and effort | Claim / last confirming evidence / next event |
|---|---|---|---|---|
| task-0 | smrpg-board `local_41a1f11f-bbab-4b02-ac69-921d17015080` | `tools/board/` | Sonnet medium / observed Sonnet 5.5 medium | Done: board https://claude.ai/artifact/7EVDkCV1KPhW9VRBeZKYTE (db `items`). Look approval pending. |
| task-1 | smrpg-extract `local_8002ca73-d772-4b26-89e1-b15956ec4ab4` | `tools/extract.py` | Sonnet low | Verified; lead rechecked ExeFS hashes. Released. |
| task-2 | smrpg-il2cpp `local_5497e2ca-368e-4637-a2f3-71a0076a47b5` | `tools/il2cpp/`, `docs/analysis/il2cpp.md` | Sonnet high | Verified 2026-11-24 (il2cpp.md; Il2CppDumper 100 percent match; lead rechecked 2 addresses). Released. |
| task-3 | smrpg-survey `local_866547cc-3341-42a9-bdcc-61422d9c1a77` | `tools/survey/`, `docs/analysis/platform-survey.md` | Sonnet medium | Verified 2026-11-24 (docs/analysis/platform-survey.md). Released. |
| task-4 | smrpg-design `local_232769fd-4908-4c20-b89c-61878230e042` | `docs/RECOMPILER.md`, `tools/spike/` | Opus high | Verified 2026-11-24 (lead reran decode check: 10,898 insns, 0 mismatches). Released. |
| task-5 | smrpg-lifter `local_94dcdf5d-9779-4f48-a4a4-dd73ce1b5c3e` | `tools/lifter/`, `runtime/` (core), `tools/rtprep/` | Sonnet high | Verified 2026-11-24 (lead reran test_lifter: 37,920 oracle cases 0 mismatches; real boot reached sm: with no interpreter misses). Released. |
| task-10 | smrpg-shaders `local_90bda3a8-33d5-47d1-90a9-ffba235350d3` | `tools/shaderxlate/`, `docs/SHADER_CACHE.md` | Opus high | Running. |
| task-11 | smrpg-textures `local_6d4697db-c4b5-43f6-a931-1fe9a9d43954` | `runtime/gfx/texture/` | Sonnet high | Verified 2026-11-24 (lead reran texture_unit: 19 pass 0 fail; own code, no reuse). |
| task-12 | smrpg-nvn `local_88602936-2a84-4f97-bf97-07c413d42d25` | `runtime/nvn/` | Sonnet high | Verified 2026-11-24 (lead reran nvn_unit: 2161 checks 0 failed; 263 of 265 procs, 2 stubs). Waits on task-5 wiring and task-10 cache. |
| task-7 | smrpg-bugs `local_6d272edb-9ebe-457e-88ae-7e61663e7af4` | `docs/analysis/known-bugs-1.0.1.md` | Sonnet medium | Verified with Reddit pass (about 30 threads; lead spot-checked 1ji4ath). Released. |
| task-8 | smrpg-retools `local_3f1d12dc-77cb-41dd-90eb-0d76b6584edf` | `docs/analysis/re-tools.md`, `F:\AI\Recomps\research_notes\ai-re-tools-2026-11.md` | Sonnet medium | Verified (re-tools.md, shared note). Released. |
| task-9 | smrpg-priorart `local_56d775aa-ae3d-4427-8281-1f1c2da48f99` | `docs/analysis/prior-art.md` | Sonnet medium | Verified 2026-11-24 (prior-art.md; lead rechecked the ElKiro repo via gh). Released. |
