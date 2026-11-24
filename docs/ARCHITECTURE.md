# Architecture (provisional; task-4 turns this into the recompiler design)

## What the original is (verified 2026-11-24)
- Program `0100BC0018138000`, update v65536. ExeFS: `main` (28.4 MB NSO: Unity 2021.3.15f1 player, IL2CPP-generated game code, libil2cpp, CRIWARE), `subsdk0` (3.4 MB, identity TBD in task-3), `sdk` (Nintendo SDK), `rtld`.
- RomFS: Unity `Data/` (globalgamemanagers, level0, resources.assets, sharedassets0), Addressables bundles in `StreamingAssets/aa/Switch` (about 1,277 bundles), `movies/*.usm` (CRI Sofdec), `sound/`, `data_tbl/`, `script/`.
- `Data/Managed/Metadata/global-metadata.dat`: IL2CPP metadata v29, unencrypted. Every C# type, method, field and string literal name is recoverable.

## Stage 1: static recompilation (playable oracle)
| System | Owns | Must not know about |
|---|---|---|
| Input pipeline (`tools/extract*`) | Dump to extracted files on A:, hash manifest | Anything downstream |
| Analysis (`tools/il2cpp`, Ghidra project on A:) | Function list, names, types, jump tables, origin classes (game, Unity, il2cpp runtime, CRI, nn, libc++) | Code generation |
| Lifter (our own) | ARM64 (A64, NEON, FP, atomics) to C, one function per guest function, indirect-call dispatch | OS services, graphics |
| Runtime core | Guest memory map, threads, TLS, sync, exceptions/unwinding (IL2CPP uses C++ exceptions), stack visibility for the Boehm GC | Game semantics |
| HLE OS | Horizon SVCs and `nn::` services: fs (RomFS, saves), hid, audio, time, settings, account, applets | Rendering details |
| Graphics | NVN to Vulkan, Maxwell shader translation (or a Unity GfxDevice-level path; task-4 decides) | Game logic |
| Audio/video | Audio renderer, CRI Sofdec movie playback | Game logic |
| Host shell | Window, input, settings, options menu, upgrade switches | Guest internals |

## Stage 2: decompilation
- Game layer (Assembly-CSharp and friends) rewritten as readable source in the D-003 language, named from metadata.
- Each function is swapped into the stage 1 build and checked by differential tests: recorded input routes, per-frame state hashes, captures.
- The Unity API surface the game actually uses is reimplemented natively (scene graph, animation, UI, Addressables, text). Assets are read from the user's dump at first run and converted to our formats on A: or the user's build folder.

## Contracts
- Guest addresses are module-relative offsets in docs and maps; the NSO import base is recorded per tool.
- Every map (functions, names, jump tables) is a generated TSV on A: plus the generator in this repo; never hand-edited.

## Testing
- Logic tests: Python `unittest`-style scripts writing JSON reports, run by `tools/game_check.py`.
- Scenario tests (from M1): recorded input route plus state hash and captures, outputs to `evidence/` (captures ignored by Git).
