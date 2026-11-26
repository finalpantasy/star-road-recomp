# Rust function swaps

The Windows game build links the `smrpg_decomp` static library from the `decomp` Cargo workspace. Rust 1.96.0 exports three integer leaves through the x64 C ABI. The crate has no dependencies.

| Guest address | IL2CPP method |
|---|---|
| 0x710213e820 | System.Math.Min(int, int) |
| 0x710213e670 | System.Math.Max(int, int) |
| 0x71018ba2c0 | Common.CommonRoutine.is_in(int, int, int) |

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

The startup test calls each registered entry with 10,000 seeded random inputs and 125 signed boundary combinations. Counters reset before the guest starts. The log check requires a positive live count for each address and zero differences in both phases.
