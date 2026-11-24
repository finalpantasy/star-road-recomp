---
id: task-8
title: Evaluate new AI reverse-engineering tools for this and future recomps
status: Ready
---
## Problem before solution
The owner found ten tools (Kuna, ReAgent, DecompDecomp, Game Decomp Compendium, ReVa, Binary Ninja, IDA Pro MCP, PyGhidra MCP, Ghidra MCP Server, decomp.wtf). Unknown: which help an ARM64 IL2CPP static recomp and later decomp, which are mature, license and cost, Windows support, MCP fit with Claude Code.
## Outcome and scope
`F:\AI\Recomps\research_notes\ai-re-tools-2026-11.md` (shared across recomps) and a short pointer section in `docs/analysis/re-tools.md` here. Per tool: what it does, maturity (stars, last commit, releases), license, cost, platforms, ARM64/AArch64 and NSO support, MCP integration, what it would replace or add to our Ghidra plus own-tools route, risks, and a verdict (adopt now, trial, watch, skip) for this project and for future recomps. Also compare against what is already installed (REA skill `reverse-engineer-anything`, Ghidra 12.1.2 with SwitchLoader under the New Pokemon Snap tools folder, the `matching-decomp`, `decomp-code-rules`, `permute`, `matcher` skills in `~/.claude/skills`). Non-goals: installing or running any tool.
## Dependencies and ownership
None. Owns the two docs above only.
Execution: subchat, Sonnet, medium effort (web research and comparison).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | All ten tools covered with source links and a verdict | Doc review | Doc | NOT_RUN |
| AC2 | Top 3 trials named with the exact install steps the owner would approve | Doc review | Doc | NOT_RUN |
## Human gates
Any download or install needs the owner's yes.
## Recovery and return condition
Docs only. Return at done.
## Approach and commands
Web pages and GitHub repo pages only (`gh` read calls fine). No sign-ins, downloads, installs or posts.
## Log and handoff
List pages that failed to load.
