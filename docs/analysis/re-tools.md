# AI reverse-engineering tools: pointer

Full evaluation (shared across recomps): `F:\AI\Recomps\research_notes\ai-re-tools-2026-11.md` (task-8, 2026-11-24). Read-only research; nothing installed.

## Verdicts for this project

| Tool | Verdict |
|---|---|
| Kuna | TRIAL (has a Ghidra 12.1.2 extension release; Apache-2.0) |
| ReVa | TRIAL (Ghidra MCP, has a 12.1 build; Apache-2.0) |
| Game Decomp Compendium | TRIAL, read-only clone, no `install.sh` |
| ReAgent | WATCH until the decomp phase, then try on one subsystem |
| PyGhidra MCP | WATCH (REA already wraps PyGhidra) |
| Ghidra MCP Server (bethington) | SKIP for now (needs Ghidra 12.2 and Java 25; we have 12.1.2) |
| DecompDecomp | SKIP (compiler-matching, alpha, AGPL, no Switch) |
| Binary Ninja | SKIP (paid; official MCP not in Free or Personal) |
| IDA Pro MCP | SKIP (needs a paid IDA Pro license) |
| decomp.wtf | SKIP (author archived it 2026-10-15; no Switch) |

Our matching-decomp skills need an original compiler and objdiff, so they stay for future N64/PS2 recomps, not IL2CPP Switch code.

Every install needs the owner's yes. Exact trial steps are in the full note.

## Part 2 pointers (expanded list)

- DecBench claim (Kuna 51.4 / Hex-Rays 49.1 / angr 47.5): not verified, table unreadable; DecBench is run by Kuna's own lab.
- Kirikae (https://github.com/Cor3Duo/kirikae): exists, Rust, MIT or Apache-2.0, created 2026-10-23, v0.1.2, early preview. Its progress file: services 417/1302, SVCs 45/124, GPU ioctls 58/138, shader instructions 82/146. WATCH and borrow ideas (shared IR with oracle tests, interpreter fallback with coverage profile, runtime coverage report, NVN shadow mode). It does not document exceptions, GC scanning or atomics, the open task-4 items.
- Names not real as given: `mips2c/m2c` (is `matt-kempster/m2c`), `mkst/reccmp` (is `isledecomp/reccmp`).
- decomp.dev is sunsetting; decomps.net returned HTTP 522; recompendium.com is a stub.
- Differential testing of lifted AArch64: our own IR semantics plus Unicorn (and a second FP/NEON oracle), Z3 per instruction, angr sampled. Matching tools (objdiff, permuter, m2c, splat, reccmp) are for future N64/PS/GC/PC decomps.
- Symbols: Il2CppInspectorRedux or Il2CppDumper (idle since 2024-08).
- Workbench proposal: not now; build the per-function status table and CLI as a by-product.
- Task-4 change list and top 5 trials with install steps: Parts 2.8 and 2.9 of the full note.
