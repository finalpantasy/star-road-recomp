# Reuse ledger (D-009)

Reused code lives only in the gitignored `reuse/` folder (and on A:). It is never committed or pushed. Each row is removed when our own rewrite replaces it and passes the same tests with the reused version as the oracle. Public release requires this table to be empty.

| Piece | Source (project, commit, path) | Origin license | Used by | Rewrite card | Status |
|---|---|---|---|---|---|
| Phase A shader cache: the Ryubing shader translator, run (not copied) through the Snap ShaderHarness `--nvn-serve` binary; our own driver `reuse/shaders/ryu_cache.py` moves its descriptors to the docs/SHADER_CACHE.md layout. Output `A:\AI\Recomps\smrpg-work\shaders\cache\reuse-v1\` | ryubing-ryujinx 4d5a19d3 `src/Ryujinx.Graphics.Shader`; new-pokemon-snap 1e7a982 `scripts/shader_harness/Program.cs` | MIT (Ryubing); Snap harness is the owner's own code | task-12 (runtime shader cache) | task-10 Phase B (`tools/shaderxlate/`, validated against this cache) | Active |
