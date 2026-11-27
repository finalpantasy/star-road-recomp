# Rust function swaps

The Windows game build links the `smrpg_decomp` static library from the `decomp` Cargo workspace. Rust 1.96.0 exports twenty-eight functions through the x64 C ABI (task-29 batch 1, task-40 batch 2, task-41 batch 3, task-42 batch 4, task-43 batch 5, task-44 batch 6). The first twenty-three are leaves: each takes guest x0 to x2 as u64 and returns the new x0. The last five call other leaves (see "Functions that call ported leaves") and edit the guest register file in place. The crate has no dependencies.

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
| 0x71002cdf10 | Unity native, zero 12 bytes at x0 (writes memory, unnamed) |
| 0x7101451240 | CRI native, store x0 twice at x2 and w1 at x2+0x10 (writes memory, unnamed) |
| 0x7101347ed8 | Unity native setter, w1 to x0+4 (writes memory, unnamed) |
| 0x7101e357b0 | Battle.BattleCharacterAnimator.set_idle_id(int, int) (writes x0+0x54 and +0x58) |
| 0x7101a0aa40 | Flag.off() (clears the byte at x0+0x10) |
| 0x71005b3d50 | Unity native, x2 = 8 then tail call to the array element address leaf (calls a leaf) |
| 0x71005b3d70 | Unity native, x0 = *(u64 *)element_address(x0, x1, 8) (calls a leaf, stack frame) |
| 0x71005ae5a0 | Unity native, x0 = object ? (field = null-safe *(object + 0x10)) ? *(u32 *)(field + 8) : 0 : 0 (calls a leaf, stack frame) |
| 0x7101cccaf0 | Unity native, w1 == 0x81d ? 1 : is_in(w1, 0x7f8, 0x7fb) (tail call to a leaf) |
| 0x71001919f0 | Unity native, x0 = (*(u64 *)(x0 + 0x10) == x1) (tail call to a leaf) |

`runtime/decomp/leaves.def` pairs each address with its lifted chunk and Rust symbol. CMake rebuilds those two chunks in its private build directory. It renames each original definition and keeps direct calls, tail calls and guest table entries pointing to the public wrapper. Startup checks the registered entry for every enabled swap.

`SMRPG_DECOMP=on` is the default (owner accepted task-29, 2026-11-26) and runs the Rust leaves. Use `off` to run the lifted body. In `ab` mode the wrapper runs Rust on a copy of the register context, runs lifted C on the live context and compares every byte. Each mismatch log identifies the guest address and the first differing byte. Leaves that store use the shadow comparison described under "Leaves that write guest memory".

## Leaves that read guest memory

Guest memory is identity mapped: a guest address is a host address (`runtime/ir_ops.h`, `MLD` and `ir_ld`, relaxed atomic loads). Rust reads through the same relaxed atomic loads (`guest_ld64` and `guest_ld32` in `decomp/smrpg_decomp/src/lib.rs`), so there is no separate mapping. Only load-only leaves qualify (no stores, calls, atomics, exclusive loads, floating point or lazy static init), because then both sides leave memory unchanged and the byte comparison stays valid. A concurrent writer between the Rust read and the lifted read could show up as a false difference; none did in the 200 s run.

The selftest needs no game data for the pointer leaves: `reads_x0_pointer` in `runtime/decomp/swap.c` points x0 at a private 64-word fake region (a mix of zero, small and full-width words) or at null. The global slot leaves read the real mapped slot, which both bodies see identically. `cargo test --release --locked --offline` in `decomp/` also checks the two pointer leaves against a fake region.

## Leaves that write guest memory

A store leaf is declared with `SLEAF(chunk, address, symbol, base_reg, offset, bytes)` in `runtime/decomp/leaves.def`: the footprint is `bytes` bytes (at most 32) at `x[base_reg] + offset`, taken from the decoded instructions. The Rust symbol takes a fourth argument, `delta`, added to every store address (`guest_st64`, `guest_st32` and `guest_st8` in `decomp/smrpg_decomp/src/lib.rs`, relaxed atomic stores like `ir_st`). The game passes 0 (`run_rust` in `swap.c`).

In `ab` mode `compare_store` in `runtime/decomp/swap.c` copies the footprint into a private shadow buffer with the same low 4 address bits, runs Rust on a register copy with `delta` set so its stores land in the shadow, then runs lifted C on live memory, so live memory ends exactly as the lifted body leaves it. It then compares all registers and the footprint bytes (live against shadow). A leaf with a footprint gap leaves the gap bytes equal on both sides. Residual race: another thread writing the footprint between the copy and the final compare would show a false difference (never a game corruption, because Rust never touches live memory). None appeared.

The selftest points the base register into a private 64-word fake region (never game memory) and requires the footprint to change in at least one call, so a pair that both skip the store cannot pass. Store leaves may not call, load from the footprint, use atomics, floating point or a write barrier, or write any register other than x0; the scan keeps only leaves of stores whose base and value registers are x0, x1, x2 or xzr. `cargo test` checks the written bytes with a fake region.

## Functions that call ported leaves

A function whose every callee is already a Rust leaf can be ported whole (task-44). `leaves.def` declares it with `CLEAF(chunk, address, symbol)`. The Rust symbol takes one argument, a pointer to the guest register file (`Ctx.x`, 32 u64 with `x[31]` = sp), and edits it in place, so every register the lifted body changes is reproduced, not just x0 (these functions set x1 to x3 and x30). It calls the Rust callee directly, so no callee runs twice on live memory. Only register and load-only callers qualify so far (no stores, write barriers, atomics, exclusives, floating point, lazy statics or indirect calls); the scan keeps at most about 20 operations and only `bl` or tail-call targets that are in `leaves.def`.

What `ab` compares (`compare` in `runtime/decomp/swap.c`): Rust runs on a copy of the whole `Ctx`, lifted C on the live one, then `memcmp` over the entire `Ctx` (x0 to x30, sp, v0 to v31, flags, pc, exception fields, scratch). Guest stack: the lifted prologue pushes x29 and x30 below sp and the epilogue pops them, so sp, x29 and the callee-saved registers end unchanged and the Rust version leaves them alone. The pushed bytes sit below the final sp and are dead after the return, so they are not compared; the comparison does cover sp and every register. One lifter artifact is copied on purpose: before each `bl` the lifted body stores the return address in the context x30 and never restores it, so the Rust version writes the same stale x30 (a wrong value there is caught: a mutation test that changed one return address failed the selftest). In `ab` mode the lifted caller still goes through the callee wrappers, which are compared as usual, so the callee live counters in the summary include calls made by the callers.

Selftest: each caller gets a private fake region and a private 16-byte aligned fake stack (`prepare_caller` in `swap.c`), with a pending exception cleared (`c.exc = 0`, lifted callers return early otherwise). The shape per function: array index and element address inside the region, a null or valid object-to-field chain, w1 at and around 0x81d and 0x7f8 to 0x7fb, an object whose +0x10 word equals x1 half the time. Callee counters are restored after each caller selftest, and a difference found in a callee fails the run, so selftest calls never count as live calls.

## Reproduce the comparison

Run from the repository root with the game build toolchain on PATH and write access to the shared work directory.

```powershell
python -I tools/rtprep/heavy_lock.py -- python -I tools/rtprep/build_game.py
python -I tools/rtprep/heavy_lock.py -- python -I tools/rtprep/run_game.py --seconds 90 --log task29_ab --env SMRPG_DECOMP=ab --env SMRPG_DECOMP_SELFTEST=1 --env SDL_AUDIO_DRIVER=dummy --env SDL_AUDIODRIVER=dummy -- --continue
python -I runtime/decomp/check_log.py A:/AI/Recomps/smrpg-work/build/logs/task29_ab.log
python -I tools/rtprep/heavy_lock.py -- python tools/game_check.py run --root .
```

The startup test calls each registered entry with 10,000 seeded random inputs and every combination of 25 boundary values (15,625). Counters reset before the guest starts. The log check requires a positive live count for each address and zero differences in both phases.
