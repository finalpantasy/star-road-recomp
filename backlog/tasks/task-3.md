---
id: task-3
title: Platform survey (render pipeline, shaders, textures, audio, video, saves, services, frame rate)
status: Ready
---
## Problem before solution
Stage 1 graphics strategy and stage 2 asset pipeline depend on facts nobody has checked yet. Facts: strings for CRIWARE, ADX2 and FMOD appear in `main`; movies are `.usm`; Addressables bundles under `StreamingAssets/aa/Switch`.
## Outcome and scope
A short evidence-backed report answering: render pipeline (built-in or URP, from globalgamemanagers / bundle shader names); shader storage format in Switch bundles (NVN binary, GLSL, or other); texture formats and swizzling; audio middleware actually used at runtime (CRI ADX2 acb/awb, FMOD, or Unity audio; `sound/` contents); movie codec; `data_tbl/` and `script/` formats; save data layout hints; full `nn::` import list grouped by service; `subsdk0` identity; NVN entry points; target frame rate and time-step hints (Application.targetFrameRate, vsync settings in globalgamemanagers / boot.config). Non-goals: decoding everything; writing converters.
## Dependencies and ownership
Inputs ready on A:. Owns `docs/analysis/platform-survey.md`, `tools/survey/`. Writes under `A:\AI\Recomps\smrpg-work\analysis\survey\`. Read-only on extracted files.
Execution: worker subagent, Sonnet, medium effort (research and light parsing).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Every question above answered with a fact and its evidence path, or marked UNKNOWN with the next step | Report review | Report | NOT_RUN |
| AC2 | nn import list complete (count matches dynsym imports) | Script output | TSV on A: | NOT_RUN |
## Human gates
No package installs or downloads (UnityPy etc. need the owner's yes). Small own parsers are fine.
## Recovery and return condition
New files only. Return at done or blocked.
## Approach and commands
Python 3 `-I`. Bundles are UnityFS (LZ4/LZ4HC chunks); a pure Python LZ4 block decoder is acceptable.
## Log and handoff
Record evidence paths and unknowns.
