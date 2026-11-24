---
id: task-2
title: IL2CPP metadata recovery and full function inventory
status: Ready
---
## Problem before solution
Facts: `global-metadata.dat` is v29, unencrypted (magic AF1BB1FA). `main` is an NSO (magic NSO0), MOD0 present. Hypothesis: Il2CppCodeRegistration and Il2CppMetadataRegistration are findable in `main` .data by pattern, giving method pointers for every C# method.
## Outcome and scope
Every function in v1.0.1 `main` gets a name and origin class. Outputs: our own metadata v29 parser (`tools/il2cpp/`), NSO to ELF converter if needed, generated TSVs on A: (types, methods with RVAs, string literals, generic method instances, origin class per function), and a Ghidra import script for names and C# struct types. Non-goals: decompiling bodies, Ghidra auto-analysis runs (heavy; later card).
## Dependencies and ownership
Inputs ready on A:. Owns `tools/il2cpp/`, `docs/analysis/il2cpp.md`. Writes under `A:\AI\Recomps\smrpg-work\analysis\il2cpp\`.
Execution: worker subagent, Sonnet, high effort (binary format work, must be exact).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Parser reads all metadata tables; counts reported (images, types, methods, fields, strings) | Run parser | Counts in doc | NOT_RUN |
| AC2 | CodeRegistration and MetadataRegistration located; method pointer count matches metadata method count rules | Run locator | Offsets and counts | NOT_RUN |
| AC3 | 20 spot-checked methods: name to address maps to a plausible function start (prologue, inside .text) | Disassemble spot checks (llvm-objdump) | Table in doc | NOT_RUN |
| AC4 | Origin classification for all .text functions with method counts per class | Generated TSV | Summary table | NOT_RUN |
## Human gates
Downloading Il2CppDumper or Cpp2IL for a cross-check needs the owner's yes; do not download.
## Recovery and return condition
Outputs are new files only. Return at done or blocked.
## Approach and commands
Python 3 `-I`. LLVM is at `C:\Users\the owner\AppData\Local\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_*` (llvm-objdump supports aarch64).
## Log and handoff
Record source hashes, commands, counts, failed hypotheses.
