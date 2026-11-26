# Rust function swaps

The Windows game build links the `smrpg_decomp` static library from the `decomp` Cargo workspace. Rust 1.96.0 exports eighteen leaves through the x64 C ABI (task-29 batch 1, task-40 batch 2, task-41 batch 3, task-42 batch 4). Each takes guest x0 to x2 as u64 and returns the new x0. The crate has no dependencies.

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
| 0x71005b3a20 | Unity native, null-safe load: x0 == 0 ? 0 : *(u64 *)(x0 + 0x10) (reads memory) |
| 0x71011eeeb0 | global slot 0x7103ded330 plus 0x18 (reads memory, unnamed) |
| 0x71011ef830 | global slot 0x7103ded330 (reads memory, unnamed) |
| 0x71005b8b00 | global slot 0x7103d823f8 (reads memory, unnamed) |
| 0x710200fd30 | System.String.IsNullOrEmpty (reads the length at +0x10) |

`runtime/decomp/leaves.def` pairs each address with its lifted chunk and Rust symbol. CMake rebuilds those two chunks in its private build directory. It renames each original definition and keeps direct calls, tail calls and guest table entries pointing to the public wrapper. Startup checks the registered entry for every enabled swap.

`SMRPG_DECOMP=on` is the default (owner accepted task-29, 2026-11-26) and runs the Rust leaves. Use `off` to run the lifted body. In `ab` mode the wrapper runs Rust on a copy of the register context, runs lifted C on the live context and compares every byte. Each mismatch log identifies the guest address and the first differing byte. These leaves have register-only effects. A leaf that writes guest memory needs an isolated copy of the affected memory before it can use this comparison.

## Leaves that read guest memory

Guest memory is identity mapped: a guest address is a host address (`runtime/ir_ops.h`, `MLD` and `ir_ld`, relaxed atomic loads). Rust reads through the same relaxed atomic loads (`guest_ld64` and `guest_ld32` in `decomp/smrpg_decomp/src/lib.rs`), so there is no separate mapping. Only load-only leaves qualify (no stores, calls, atomics, exclusive loads, floating point or lazy static init), because then both sides leave memory unchanged and the byte comparison stays valid. A concurrent writer between the Rust read and the lifted read could show up as a false difference; none did in the 200 s run.

The selftest needs no game data for the pointer leaves: `reads_x0_pointer` in `runtime/decomp/swap.c` points x0 at a private 64-word fake region (a mix of zero, small and full-width words) or at null. The global slot leaves read the real mapped slot, which both bodies see identically. `cargo test --release --locked --offline` in `decomp/` also checks the two pointer leaves against a fake region.

## Reproduce the comparison

Run from the repository root with the game build toolchain on PATH and write access to the shared work directory.

```powershell
python -I tools/rtprep/heavy_lock.py -- python -I tools/rtprep/build_game.py
python -I tools/rtprep/heavy_lock.py -- python -I tools/rtprep/run_game.py --seconds 90 --log task29_ab --env SMRPG_DECOMP=ab --env SMRPG_DECOMP_SELFTEST=1 --env SDL_AUDIO_DRIVER=dummy --env SDL_AUDIODRIVER=dummy -- --continue
python -I runtime/decomp/check_log.py A:/AI/Recomps/smrpg-work/build/logs/task29_ab.log
python -I tools/rtprep/heavy_lock.py -- python tools/game_check.py run --root .
```

The startup test calls each registered entry with 10,000 seeded random inputs and every combination of 25 boundary values (15,625). Counters reset before the guest starts. The log check requires a positive live count for each address and zero differences in both phases.
