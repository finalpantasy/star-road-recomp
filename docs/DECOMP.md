# Rust function swaps

The Windows game build links the `smrpg_decomp` static library from the `decomp` Cargo workspace. Rust 1.96.0 exports twenty-eight functions through the x64 C ABI (task-29 batch 1, task-40 batch 2, task-41 batch 3, task-42 batch 4, task-43 batch 5, task-44 batch 6). The first twenty-three are leaves: each takes guest x0 to x2 as u64 and returns the new x0. The last five call other leaves (see "Functions that call ported leaves") and edit the guest register file in place. The crate has no dependencies. Task-45 adds up to 300 more and task-46 up to 3,778 more, generated in bulk (see "Bulk generator").

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

## Bulk generator (task-45, task-46)

`tools/decompgen/` ports exact trivial-function shapes in bulk. `python -I tools/decompgen/generate.py` reads every main-module function of at most five instructions (the lifter's inventory and decoder, `tools/lifter/dec.py`), matches a closed pattern set (`patterns.py`) and ranks the matches by direct call sites (`bl` and `b` to the entry). Anything that is not the exact shape is skipped. Patterns, each with one Rust template:

| Pattern | Shape | Row |
|---|---|---|
| empty | `ret` | LEAF |
| const | `movz`, `movn`, `movk` sequence into x0 or w0, or `orr` from the zero register, then `ret` | LEAF |
| get | `ldr`, `ldrb`, `ldrh`, `ldrsb`, `ldrsh`, `ldrsw` into x0 or w0 from `[x0..x2, #imm]`, then `ret` | GLEAF |
| get_nullsafe | `cbz x0, +8; ldr x0 or w0, [x0, #imm]; ret` | NLEAF |
| set | `str`, `strb`, `strh` of x0..x2 or the zero register to `[x0..x2, #imm]`, then `ret` (no barrier, no call) | SLEAF |
| add_imm | `add` or `sub` x0 or w0, `x0..x2`, `#imm` without flags, then `ret` | LEAF |
| mov_reg | `orr x0 or w0, xzr, x1 or x2`, then `ret` | LEAF |
| set_bool | `and wT, wS, #1; strb wT, [xN, #imm]; ret` (T a scratch register 3..28, S and N 0..2) | SLEAF |
| vget (task-50) | `ldr sN or dN, [x0..x2, #imm]; ret` (N = 0..15) | VLEAF 1 |
| vset (task-50) | `str sN or dN, [x0..x2, #imm]; ret` (N = 0..15) | VLEAF 2 |
| vconst (task-50) | `fmov sN or dN, #imm`, `fmov sN, wzr`, `fmov dN, xzr`, `movi vN.2d, #0`, then `ret` (N = 0..15) | VLEAF 3 |
| get2 (task-50) | `ldr xT, [x0..x2, #a]; ldr{,b,h,sb,sh,sw} x0 or w0, [xT, #b]; ret` (T = 0 or 8..17, no null check) | DLEAF |
| vget2 (task-50) | `ldr xT, [x0..x2, #a]; ldr sN or dN, [xT, #b]; ret` (T = 8..17) | VLEAF 4 |
| get_cmp (task-50) | `ldr{,b,h} xT or wT, [x0..x2, #imm]; cmp xT or wT, #k (k <= 15) or xM (M = 1 or 2); cset w0, eq or ne; ret` (T = 8..17) | GLEAF |

Not matched on purpose: write-back or register-offset addressing, q-register (16 byte) and v16..v31 accesses, pair loads and stores (`ldp s0, s1`), `fmov` between a vector and a general register other than the zero register, flag-setting instructions other than the `cmp` plus `cset` boolean getter, anything with a call or branch other than the null check. The semantic proof of each template is the A/B selftest against the lifted C for every generated function: sign and zero extension, w against x writes clearing the upper half, and the byte, halfword, word and doubleword store widths are all compared byte for byte over the whole register context. The boolean setter (task-46) also leaves the low bit in x8. The lifted reference never exposes it: at `ret` the lifter flushes only x0..x7 and x19..x29 to the context (`MASK_RET` in `tools/lifter/lift.py`), so x8 is a dead scratch register, and a first version that wrote x8 through a register-file row was caught by the whole-context selftest (`regs=1`). The port is a plain store of the low bit with the ordinary `SLEAF` store shadow; the whole context stays compared. `python -I tools/decompgen/test_decompgen.py` pins which instruction words are accepted and which near misses are refused.

Float and SIMD rows (task-50). Context: `Ctx.v[32]` is part of the A/B `memcmp`, so the vector registers are compared like every other register. The lifted reference (`OP_ldv` in `runtime/ir_ops.h`, `sa_mk` for `fmov`) writes `{value, 0}`: `ldr s0` zero extends the 32 bit load to 64 bits and clears lane 1, `ldr d0` loads 64 bits and clears lane 1, `fmov` and `movi` write `{bits, 0}`; so a generated load or constant returns the low 64 bits and the wrapper builds `{bits, 0}`. A store reads the low 32 or 64 bits of the register. At `ret` the lifter flushes only v0..v15 to the context (`MASK_RET`), so v16..v31 are never matched. Row: `VLEAF(chunk, address, symbol, vkind, vreg, base_reg, offset, bytes, offset2)` with vkind 1 load, 2 store, 3 constant, 4 load through a field. The Rust symbol takes `(x0, x1, x2, low 64 bits of the vector register, store redirect)` and returns the new low 64 bits (a store returns 0); `run_vec` in `swap.c` writes the register, leaves x0 alone, and for a store reuses the shadow compare. `DLEAF(chunk, address, symbol, base_reg, offset, bytes, offset2)` is the integer load through a field: the selftest stores into the first slot a pointer to another slot of the fake region (minus `offset2`). The scratch register of the two-level and boolean shapes (x8..x17) is dead at `ret` by the same `MASK_RET` argument as set_bool, so only x0 is produced. The selftest now fills v0..v31 with random values from a separate stream (a constant 0xa5 fill would hide lane bugs) and, for load rows, sets x1 and x2 equal to the field a quarter of the time (equal in the low 32 bits only one time in eight) so the `cmp` boolean getters see both outcomes. Races: a load row reads live memory twice (Rust, then lifted C) and another guest thread can write the field in between. One `ldr w0, [x0, #0x68]` (0x7100492560, a task-45 row unchanged since) showed 1 difference in 77,477 live calls in one run (rust 0, lifted 0x64) and none in the five earlier runs or the next two. `compare` now, for load rows only, reruns the identical pair from the saved input once on a difference: a repeat is still a `decomp mismatch`; an agreeing retry is logged as `decomp raced addr=...`, counted, and shown by `check_log.py --summary` (`raced loads`); the selftest fails on any race (its region is private). Stores keep the shadow compare; register-only rows are deterministic and never retried.

New row kinds in the manifest: `GLEAF(chunk, address, symbol, base_reg, offset, bytes)` and `NLEAF(...)` (null-safe) declare loads. They call like `LEAF`; the selftest points `x[base_reg] + offset` into a private fake region (NLEAF also passes null one time in eight). Generated stores reuse `SLEAF` with its shadow comparison. `swap.c`, `game.cmake`, `wrap_chunk.py` and `check_log.py` read the hand-written rows from `leaves.def` followed by the generated rows (`game.cmake` concatenates them into `manifest.def` in the build directory and rewrites it only when the text changes).

Where the output lives: `A:\AI\Recomps\smrpg-work\decompgen\` (`gen.rs`, `gen.def`, `gen.index.tsv`, `gen.report.txt`), not in Git. The files are derived from the game binary (addresses, field offsets, IL2CPP method names), the same reason lifted chunks are not committed, and the set grows with the flags. The generator, the templates, the tests and the hooks (`decomp/smrpg_decomp/build.rs`, `game.cmake`) are in Git. `build.rs` includes `gen.rs` through `SMRPG_DECOMPGEN_DIR` (default the A: directory above); without the file the crate still builds with the hand-written functions only. `game.cmake` runs the generator once when `gen.def` or `gen.rs` is missing. Regenerate by hand after changing a flag.

```powershell
python -I tools/decompgen/generate.py --stats-only            # counts only
python -I tools/decompgen/generate.py                          # default: top 300 by call sites, at most 12 lifted chunks
python -I tools/decompgen/generate.py --limit 1000 --max-chunks 40
python -I tools/decompgen/generate.py --limit 100000 --max-chunks 0   # every match (13,624 functions in 352 chunks, task-50)
python -I tools/decompgen/generate.py --limit 100000 --max-chunks 120 --chunks 0:200 --exclude-chunks 169   # the task-46 built set
python -I runtime/decomp/check_log.py --summary A:/AI/Recomps/smrpg-work/build/logs/task45_ab.log
```

The cap on lifted chunks exists because the build recompiles every chunk that holds a swapped function (about 3 MB of C each); 300 functions spread over all chunks would rebuild most of the 393. `--max-chunks 0` removes the cap. `--chunks LO:HI` keeps only chunks LO to HI-1 and `--exclude-chunks N,N` drops chunks (counts in the report stay global); `wrap_chunk.py` leaves an unchanged wrapped file alone, so a build after a manifest change only recompiles the chunks that really changed. Functions already in `leaves.def` are excluded. `check_log.py --summary` keeps the selftest and mismatch checks for every row but reports live against selftest-only counts instead of failing on a function the run never called.

Build cost (task-46, `-j 16`, BelowNormal). Compiling a wrapped chunk takes seconds to minutes (main_0169, 7.3 MB of C, ran past 8 minutes at -O2 and was killed), but the dominant cost is the final link: every wrapped chunk object adds about 4.5 MB to `smrpg.exe` (437 MB with 15 chunks, 1.13 GB with 185) and linking that took 9 minutes. A full set of 351 chunks cannot be linked inside the 10 minute limit, so the built set is the 120 best chunks (by functions plus call sites) of chunks 0 to 199 without main_0169: 3,778 of the 12,032 generated functions (the other 8,254 stay lifted; their Rust is generated and type-checked, not linked). Link time, not compile time, is the cap to attack next (for example a split link or less debug info), not the generator. The selftest cost is about 4 ms per row (3,806 rows in 15.3 s), so all 12,032 would take about 50 s; the selftest runs only with `SMRPG_DECOMP=ab` and `SMRPG_DECOMP_SELFTEST=1`, never in the default `on` mode (`on` starts in 0.35 s with 3,806 rows registered).

## Readable ports (task-51)

The bulk generator ports shapes. A readable port is a game method written as Rust a person can read, with the
game's own class, field and method names. Conventions, so later ports match:

- One module per IL2CPP class family, named for it (`decomp/smrpg_decomp/src/battle.rs` for `Battle.*`).
- A field access layer (`object.rs`): `guest_class!(Name)` declares a one-field wrapper around a guest address;
  the `GuestObject` trait reads fields (`bool_field`, `i32_field`, `u32_field`, `object_field`, `optional_field`)
  with the same relaxed atomic loads as the lifted code. Each class names its fields once, as private `const`
  offsets taken from the IL2CPP field table, and exposes accessors. The method bodies use accessors only: no
  address arithmetic, no registers. Reference fields the original dereferences blind use `object_field`; ones it
  tests for null use `optional_field`.
- Enums stay raw integers (`BadStatusKind(u32)` with associated constants) because the game stores any int.
- Where a method has several early exits, the readable function returns which test decided (`GlobeCheck`,
  `CommandCheck`) and the public bool is derived from it. The extra information is what the shim needs.
- The shim is at the bottom of the module and is the only code that sees registers. Pure leaves use a
  `leaf_shim!(symbol, Class, method)` row (`LEAF`, x0 is `this`, x1 the hidden method pointer, result in x0).
  Callers of other ported leaves use a `CLEAF` shim taking the register file. It copies the lifter's residue:
  before each `bl` the lifted body writes x1 (the zero method pointer), x8 and x30 (the return address) to the
  context and never restores them, so a path that reached a call must leave those values and a path that
  returned before the call must not. The A/B compares the whole context, so a wrong residue is a mismatch.
- Names, field offsets and addresses are in Git for hand-written ports, as they already are for the 28 leaves
  in `leaves.def`. Bulk-derived sets stay on A:. If the owner rules the readable modules out of Git, move the
  module to A: next to `gen.rs` and include it from `build.rs`.
- Selftest: `battle_object_kind` and `prepare_battle` in `runtime/decomp/swap.c` build the object graph (motion,
  character, param, status manager, buff manager) in the private fake region with edge values for every field
  read (bool bytes 0, 1, 2, 0x80, 0xff; statuses 0 to 12 and the 32-bit edges; counts at the signed edges; a null
  parent one time in eight). A new class needs a graph builder like it. `cargo test` repeats the logic against
  an independent reference written from the disassembly (the status bit mask 0x248, for one).
- Floating point goes through `fp.rs` (task-53, next section). A float port keeps the lifted operation order and
  operand order, and its selftest graph carries NaN, infinities, signed zeros and subnormals and every FPCR mode.

First readable ports: `Battle.BadStatusManager.can_command`, `Battle.BattleCharacterParam` (`is_dead`,
`is_gone`, `is_invincible`, `can_battle`, `can_targetting`, `can_act`, `can_use_any_command`) and
`MotionSpecial_Mario.can_bunnage_globe` (nine rows, task-51). Task-52 adds `Battle.BattleHit.exec_hit` (`ELEAF`, compared by replay; see the next section). Task-53 adds the float layer and `Battle.BattleCamera.adjust_camera_transform` (see "Float layer").

## Guest-call bridge (task-52)

A readable port that replaces a lifted function may still need to call lifted functions. `decomp/smrpg_decomp/src/bridge.rs`
gives it `Regs` (the register file the port edits, `Ctx.x`, with v0 to v31 following it in memory) and `Regs::invoke(target)`,
backed by `smrpg_guest_bl` in `runtime/decomp/swap.c`. `invoke` does what a lifted `bl` does: the callee runs on the
caller's own register file, so sp is the caller's sp (the callee's frame sits below it, as the lifted code's does), x30
and every other argument register are whatever the port staged, and the callee's residue stays in the file. The call is a
direct call to the registered entry (`f_<addr>(c)`, so a callee that is itself a Rust row goes through its wrapper), or the
interpreter fallback `rt_icall` when the address has no lifted entry. It returns 1 when the callee left an exception
pending; the port returns at once, as the lifted caller does. `Regs::call` stages x0 to x7 and x30 for the common case;
`Regs::set_f32` and `f32_result` move float arguments and results through v0 to v7. A port writes to the register file
only what the lifted code flushes at that call site (the lifter writes a register to the context at the first call after
it changed), because the A/B compares the whole context.

Which callees may run in `ab` mode (the choice and why). `ab` runs the port on a copy of the context and then the lifted
body on the live one, so a callee run by both runs twice on live memory. Two ways to compare, one per row kind:

- Pure callees (`CLEAF` rows). The port may call only a function on the pure allowlist: `runtime/decomp/pure.def` plus the
  ported load rows (`LEAF`, `GLEAF`, `NLEAF`, `DLEAF`, `VLEAF` that is not a store). Any other target in `ab` mode stops the run
  (exit 10, a log line naming the address) before the callee executes, so a side effect can never run twice. Each
  `pure.def` entry is proven by `python -I runtime/decomp/check_pure.py` from the lifted chunk: every store is inside the
  function's own stack frame (a pre-index push, or a non-negative offset inside the frame it allocated), no `rt_icall`,
  `rt_dispatch`, `rt_svc`, `rt_brk` or exception landing, no exclusive, acquire or release instruction, no operation outside
  the load, arithmetic, float and SIMD-arithmetic set, and every direct callee is itself pure. The comparison is the usual
  one: `memcmp` over the whole `Ctx` (x, v, flags, fpcr, fpsr, pc, exception fields, scratch). It does not compare stack
  memory below sp (dead after return) and does not compare heap memory (the proof says no callee writes it; the port itself
  must not write any).
- Callees with side effects (`ELEAF` rows, replay). The lifted body runs once on live memory, exactly as without the swap,
  and the calls it makes through traced rows (`PLEAF`) are recorded: callee address, the whole register file at the call, and
  after it. The port then runs on a copy of the entry context; its `invoke` calls do not execute, each is matched with the
  next recorded call (same callee, same x0 to x30 and v0 to v31 at the call, so wrong arguments, a wrong x30 or a wrong
  frame are found at the call), and the port receives the recorded register file after it. At the end the port's whole
  `Ctx` must equal the lifted one and every recorded call must have been used. This covers: the order and number of callee
  calls, every register at every call, the control flow between calls (as it shows in later calls and in the final registers),
  and the final register file including residue. It does not cover: what the callees do (they ran once, lifted, unchanged), the
  port reading memory after the callees ran instead of before (the port runs second, so a field the callees change between
  two reads can differ and shows as a mismatch to diagnose), a callee reached through an untraced function (the recorder
  sees only calls from the row's own body, so every callee must be a `PLEAF` row), and more than 12 traced calls per run
  (the comparison is skipped and logged). A port in a replay row writes no guest memory.
- `on` mode runs the port once with real callees and has no such limit. `off` runs the lifted body.

Selftest for both kinds runs at startup like the other rows (10,000 random plus 15,625 edge cases). A replay row's traced
callees are stand-ins during the selftest (they clobber x0 to x17 and v0 to v7 pseudo-randomly and never run the game
function, because the fake object graph cannot support the real one); the lifted body between the calls is real. The
bridge row is `0x7101b8abd0`, a thunk (`b 0x7100049adc`) that is one tail call over the pure chain `0x7100049adc` and
`0x710006b41c`; its fake graph is built in `prepare_caller`. `cargo test` repeats the port logic with a recorder that
implements the callee trait, and the register staging with a fake bridge.

Traced rows (`PLEAF(chunk, address)`, no Rust symbol) also count live calls of any lifted function in `ab` mode and run
the lifted body unchanged, which is how the candidate table in `backlog/tasks/task-52.md` got its live evidence.

## Float layer (task-53)

`decomp/smrpg_decomp/src/fp.rs` is the Rust version of the scalar float helpers in `runtime/ir_simd_a.h`, so a port can do
float arithmetic and leave the context exactly as the lifted body does. The lifted instructions call `sa_fp2`, `sa_fp3`,
`sa_fcmp`, `sa_fcvt`, `sa_fcvti` and `sa_cvtf`; each has a fast path (host SSE/FMA with error-free transformations, only when
FPCR is 0 and the operands are ordinary finite numbers) and a soft path (`sa_s_*`, the ARM pseudocode in integer arithmetic).
`fp.rs` ports the soft path line by line (`unpack`, `round`, `sum`, the NaN rules), in single and double precision, and uses
no host float operation (so MXCSR and the compiler cannot change a result or a NaN payload). `FpEnv { fpcr, fpsr }` carries the
state: FPCR is read (FZ bit 24, DN bit 25, RMode bits 23:22), cumulative FPSR flags (IOC, DZC, OFC, UFC, IXC, IDC) are OR-ed in
and never cleared. Operations: `addsub_bits`, `mul_bits`, `div_bits`, `fma_bits` and `fp3_bits` (fmadd, fmsub, fnmadd,
fnmsub), `fcmp_bits` (fcmp and fcmpe, returns NZCV), `minmax_bits` (fmax, fmin, fmaxnm, fminnm), `cvt_bits` (single and
double), `tofix_bits` (fcvtz*, fcvtn*, fcvtp*, fcvtm*, fcvta*, with fixed-point bits) and `fromfix_bits` (scvtf, ucvtf), plus
typed `f32` and `f64` wrappers (`fadd32`, `fmul32`, `fmadd32`, `fcmp32`, `fadd64`, and so on). Ports use the `Float32Ops` trait
(`fadd`, `fsub`, `fmul`); `Regs::fp_env()` and `Regs::set_fpsr()` read and write the context's FPCR and FPSR, which the shim does
around every operation because a callee may have changed the flags. Not covered, as in the C: half precision, AHP, FEAT_AFP
(AH, FIZ, NEP), trapped exceptions, `fsqrt`, `frint*`, the estimate and step instructions, and vector forms. Where the C and the ARM
pseudocode would differ, `fp.rs` follows the C; the selftest below found no such case.

Evidence that it matches the lifted helpers: `runtime/decomp/fp_selftest.c` (run at startup under `SMRPG_DECOMP=ab
SMRPG_DECOMP_SELFTEST=1`, before the row selftests) calls the C helpers and `smrpg_fp_selftest_op` on the same operands and
compares result and FPSR. It covers 14 arithmetic and compare ops, the two format conversions, float to integer in all five
rounding modes with fixed-point bits, and integer to float, in both precisions, for every pair of 37 to 45 edge operands (zeros,
denormals, rounding boundaries, extremes, infinities, quiet and signaling NaNs with payloads) in ten FPCR modes (0, FZ, DN,
FZ+DN, RMode 1, 2, 3 and three mixes), plus 60,000 random pairs per op (windowed exponents for cancellation), with a random
FPSR going in: 2,732,380 cases, 0 differences. `cargo test` holds 14 hand-computed cases (tie to even, the four rounding modes,
overflow to infinity or the largest finite number, divide by zero, NaN propagation and default NaN, denormal inputs and
results under FZ with IDC and UFC, signed zero in every mode, fused versus split multiply-add, compare flags, min and max with
zeros and NaNs, conversions, double precision).

`unity.rs` holds `Vector3`, `Quaternion`, `Camera` and `Transform`; `Vector3` arithmetic goes through `Float32Ops` in the compiled
operand order.

### BattleCamera.adjust_camera_transform

Row `ELEAF(main_0343, 7101e2cfd0, smrpg_camera_adjust_camera_transform)`, replay-compared (the six callees are `PLEAF` rows:
`Component.get_transform` main_0273, `Transform.set_eulerAngles`, `get_position`, `set_position` main_0274,
`Quaternion.Internal_FromEulerRad` and `op_Multiply` main_0364). The readable body (`BattleCamera::adjust_camera_transform`
in `battle.rs`): set the camera transform's Euler angles from `_euler_angle`; unless `_is_position_change_locked`, place the
camera at `_look_at_position - Quaternion.Euler(_euler_angle * Deg2Rad) * (0, 0, _distance)`; if there is a `_camera_shake`,
add its `offset` to the position (jump to `set_position`). The compiled code reads `_camera_shake` before the safepoint and
again for the offset; the port does the same. The shim (`LiftedCamera`) writes at each call the registers the lifted body
flushes (including `v3`, `v8` to `v10`, x8 as the Deg2Rad bits, x20 as the transform), restores x19, x20, x29, d8 to d10 and sp
at the epilogue, and lets the Quaternion and Vector3 results stay in v0 to v3 where the callees left them. `cargo test` checks the
shim against a transcription of the lifted C (`lifted_reference`, with scrambling fake callees that hash every register they are
handed) over 1,680 graphs (3 flag values, shake or none, 7 FPCR modes, edge floats): the whole register file, call order and
safepoints must be equal.

The loop back-edge safepoint (`g_gc_pending`, `gc_poll`) is `Regs::safepoint` and `smrpg_guest_safepoint` in `swap.c`: with a
collection pending it writes the one register the lifted code flushes there and `c->pc`, and polls (not in a replay trial). A
race where the lifted body saw `g_gc_pending` set and the trial did not (or the reverse) would show as a mismatch; none seen.

Build note: the first float code in the Rust archive references the MSVC marker `_fltused`; `swap.c` defines it.

What the live A/B can and cannot see (found by mutation, task-53). FPSR flags are sticky: after the game's first inexact
operation IXC is set for good, so a port that forgets IXC is invisible to the live compare. The startup selftest catches it
(the flag mutation of the card: 185,516 differences at 0.4 s). Wrong results (operand order, rounding) are caught live at the
first follow-step call. Live, the camera runs the follow step without a shake (every mismatch of the mutation runs had 6
recorded calls); the shake branches are covered by the selftest and the Rust tests, not by the game route.

## Stricter float flag check (task-54)

FPSR cumulative flags are sticky and the game sets IXC early, so a flag a port forgets can hide behind a flag that is
already set, or behind one a callee or another operation of the same call raises. Three layers now close that, all in
`ab` mode only (game behavior is unchanged: the original bits are ORed back).

1. Row level, `compare_replay` in `swap.c`. A replay row (`ELEAF`, `clear_fpsr`) starts both sides with the cumulative
   bits (IOC DZC OFC UFC IXC IDC QC, mask 0x0800009f) cleared, compares what each side raised, and ORs the original
   bits back into the live context at the end.
2. Callee flags are hidden while the compared body runs. A traced callee (`PLEAF`) runs with the cumulative bits cleared;
   what it raised is held aside (`Recording.raised`), the body's own bits are put back, and the recorded post-call
   context carries only the body's bits. The pre-call FPSR compare at every callee (`replay_call`) therefore sees only the
   flags the body itself raised before that call, and the final compare sees only the flags of its own operations after
   the last call. The held bits are ORed into the live context after the compare. Without this, the first lifted callee
   that raised IXC masked every later omission (measured: the IXC mutation of task-53 stayed invisible with the
   start-cleared compare alone).
3. Operation level, `Regs::fp_op` in `bridge.rs` and `smrpg_fp_shadow` in `fp_selftest.c`. Each float operation a shim
   runs (fadd, fsub, fmul, scvtf, fcvtzs, fcmp, single precision) also runs on the lifted C helper with the same FPCR,
   the same FPSR going in and the same operands, and the result and FPSR coming out must be equal. This sees a flag
   dropped for one sign of result even when another operation of the same call raises the same bit. A difference logs
   `decomp mismatch fp shadow ...` (so the usual "decomp mismatch" count catches it); the exit summary prints
   `decomp fp shadow checked=N differences=N`.

What the stricter check sees on the hard route: the camera's own operations raise IXC for positive inexact results (the
Deg2Rad multiplies) and, with a mutation that drops IXC for negative results, nothing is seen live because no
negative result is inexact on that route (44,890 shadow-checked operations, 0 differences); the startup selftest, which
feeds random operands and a random FPSR, still covers that sign.

## Defence stat ports (task-54)

`Battle.BattleCharacterParam.get_calc_defence` (0x71021bc270) and `get_calc_magic_defence` (0x71021bc440), 28 lifted
instructions each, in `battle.rs` (`calc_defence`, `calc_magic_defence`, one body `calc_defence_stat`). Readable
form: `_defence` (0x40, or `_magic_defence` 0x48) plus `_buff_mng._add_status[1]` (or `[3]`, an `int[]` read through
`Int32Array` in `object.rs`); for a player character (`_is_player`, byte at 0x6c) the sum is multiplied by the buff manager's
`defense_up_ratio` (0x71021c4240, or `magic_defense_up_ratio` 0x71021c4560) and cut back to an int the way the compiled
cast does it (`scale_by_ratio`: scvtf, fmul, fcvtzs toward zero, and a product of exactly +infinity gives `int.MinValue`).
A too-short array throws through 0x71000c4510. Float steps go through `Float32Convert` (`fp.rs`: scvtf, fcvtzs, fcmp on
top of `Float32Ops`). Shim `LiftedStat` stages exactly the registers the lifted body flushes (before the ratio call x0,
x8, x9, x10, x19, x29, x30 and sp; before the throw x0, x8, x9, x10, x29, x30 and sp; at the return x0, x19, x29, sp, v0
and v1). Rows: `ELEAF(main_0376, ...)` for both, traced callees `PLEAF` for the two ratio methods and the thrower.

Why these and not the damage formula. `SkillExecter.damage_calc` (0x71021ff580) is 613 instructions. `CharaSkillResultLogger.calc_damage_normal`
(303) builds the damage number display (stack structs, vector math, UI calls). `BattleCharacterParam.apply_damage_and_status_eff`
(277) is mostly logging and lazy static initialization. `get_calc_attack` (96 instructions, 18 live calls) is the same shape as
the defence methods but has the non-player branch with class-initialization checks and two garbage-collection polls, which the
shim would have to reproduce site by site. The defence methods have none of that and are the stat the damage code reads.

## Reproduce the comparison

Run from the repository root with the game build toolchain on PATH and write access to the shared work directory.

```powershell
python -I tools/rtprep/heavy_lock.py -- python -I tools/rtprep/build_game.py
python -I tools/rtprep/heavy_lock.py -- python -I tools/rtprep/run_game.py --seconds 90 --log task29_ab --env SMRPG_DECOMP=ab --env SMRPG_DECOMP_SELFTEST=1 --env SDL_AUDIO_DRIVER=dummy --env SDL_AUDIODRIVER=dummy -- --continue
python -I runtime/decomp/check_log.py A:/AI/Recomps/smrpg-work/build/logs/task29_ab.log
python -I tools/rtprep/heavy_lock.py -- python tools/game_check.py run --root .
```

The startup test calls each registered entry with 10,000 seeded random inputs and every combination of 25 boundary values (15,625). Counters reset before the guest starts. The log check requires a positive live count for each address and zero differences in both phases.

Build size fix (task-47). Cause of the 4.5 MB per wrapped chunk: the runtime target compiled wrapped chunks with `-O2 -g`, while the lifted objects have no DWARF; a wrapped `main_0001` object was 604,849 B (550 KB DWARF) against 55,333 B unwrapped with the same 53,695 B of code. `runtime/decomp/game.cmake` now adds `-g0` for wrapped chunks. 120 chunks: `smrpg.exe` 948 MB to 312 MB, rebuild plus relink 208 s. All 350 chunks with generated functions (12,028 functions, main_0169 still excluded): `smrpg.exe` 314 MB, 322 s for the full build, 12,056 rows registered. The task-46 limit on the built set no longer applies; the generator command above with `--max-chunks 0 --exclude-chunks 169` is the normal set.

Generator v2 (task-50). Six more shapes (table above): float and double getters, setters and constants, field-of-field getters (integer and float) and boolean field getters. Matches in the main module and rows built (all of them, main_0169 included): see backlog/tasks/task-50.md. 13,624 rows over 352 chunks (4 of them in main_0169, which now compiles: the whole build including it took 444 s with `-g0` and `--jobs 16`; before task-47 its wrapped compile did not finish in 8 minutes). `smrpg.exe` is 315 MB. Selftest startup grew to about 94 s for 13,652 rows, so the hidden A/B run needs `--seconds 260` with `-- --max-seconds 150` (the game watchdog starts after the selftest).
