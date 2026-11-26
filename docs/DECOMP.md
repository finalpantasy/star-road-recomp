# Rust function swaps

The Windows game build links the `smrpg_decomp` static library from the `decomp` Cargo workspace. Rust 1.96.0 exports thirteen integer leaves through the x64 C ABI (task-29 batch 1, task-40 batch 2, task-41 batch 3). Each takes guest x0 to x2 as u64 and returns the new x0. The crate has no dependencies.

| Guest address | IL2CPP method |
|---|---|
| 0x710213e820 | System.Math.Min(int, int) |
| 0x710213e670 | System.Math.Max(int, int) |
| 0x71018ba2c0 | Common.CommonRoutine.is_in(int, int, int) |
| 0x71005aca60 | array element address (x0 + sext32(x1) * x2 + 0x20) |
| 0x7101b905b0 | System.Numerics.Hashing.HashHelpers.Combine(int, int) |
| 0x7101d70190 | System.Type.op_Equality |
| 0x7101d715a0 | System.Type.op_Inequality |
| 0x7101df0f10 | System.IntPtr.op_Equality |
| 0x7101df0190 | System.IntPtr.op_Inequality |
| 0x7102021db0 | System.String.GetRawStringData (x0 + 0x14) |
| 0x7101c9d200 | UnityEngine.UIElements.VisualElement.get_computedStyle (x0 + 0x2c8) |
| 0x7101df0f80 | byte array element address (x0 + sext32(x1)) |
| 0x7100f8a7f0 | 16.16 fixed-point rounded multiply (unnamed in the method map) |

`runtime/decomp/leaves.def` pairs each address with its lifted chunk and Rust symbol. CMake rebuilds those two chunks in its private build directory. It renames each original definition and keeps direct calls, tail calls and guest table entries pointing to the public wrapper. Startup checks the registered entry for every enabled swap.

`SMRPG_DECOMP=on` is the default (owner accepted task-29, 2026-11-26) and runs the Rust leaves. Use `off` to run the lifted body. In `ab` mode the wrapper runs Rust on a copy of the register context, runs lifted C on the live context and compares every byte. Each mismatch log identifies the guest address and the first differing byte. These leaves have register-only effects. A leaf that writes guest memory needs an isolated copy of the affected memory before it can use this comparison.

## Reproduce the comparison

Run from the repository root with the game build toolchain on PATH and write access to the shared work directory.

```powershell
python -I tools/rtprep/heavy_lock.py -- python -I tools/rtprep/build_game.py
python -I tools/rtprep/heavy_lock.py -- python -I tools/rtprep/run_game.py --seconds 90 --log task29_ab --env SMRPG_DECOMP=ab --env SMRPG_DECOMP_SELFTEST=1 --env SDL_AUDIO_DRIVER=dummy --env SDL_AUDIODRIVER=dummy -- --continue
python -I runtime/decomp/check_log.py A:/AI/Recomps/smrpg-work/build/logs/task29_ab.log
python -I tools/rtprep/heavy_lock.py -- python tools/game_check.py run --root .
```

The startup test calls each registered entry with 10,000 seeded random inputs and every combination of 25 boundary values (15,625). Counters reset before the guest starts. The log check requires a positive live count for each address and zero differences in both phases.
