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

## Pre-state and store journal (task-55)

Problem. A replay row (`ELEAF`) runs the lifted body once on live memory and the port second, on a copy of the entry
registers. A port that reads memory the lifted body already changed sees the wrong state. IL2CPP lazy initialization is the
case that matters: a method tests a flag byte, calls the metadata initializer, sets the flag, and tests a class's
initialized word before it calls the class initializer. By the time the port runs, the flag is set and the word is nonzero,
so the port would skip calls the lifted body made. Re-running the callees to get the state back is not allowed (a callee
with side effects must never run twice on live memory).

Mechanism (`runtime/decomp/swap.c`, `bridge.rs` `prestate`), row kind `QLEAF(chunk, addr, port, hook)`, a replay row with a
pre-state hook:

- Before the lifted body runs, `compare_replay` calls the hook (`int hook(uint64_t *x)`). The hook calls
  `smrpg_guest_snapshot(addr, size, watch)` for every cell the port will read or store (`watch` marks a cell the port
  stores to). It returns nonzero when the state cannot be captured; then the call runs lifted only, is counted in `calls`
  and in a separate `deferred` counter (printed as `decomp live addr=... deferred=N`), and nothing is compared. This is the
  first call of a method in a process: the flag byte is still clear and the metadata slots are unresolved tokens.
- In the trial the port reads those cells with `prestate::read_u8` / `read_u32` (`smrpg_guest_pre_read`): the snapshot value
  while a trial runs, the live value otherwise (so `on` mode, where the port is the only run, is unchanged). A read after a
  callee the lifted body ran in this call changed the cell (a class initialized word after the class initializer call) is a
  live read: the port knows from its own replayed call that the initializer ran.
- The port's stores go through `prestate::store_u8` (`smrpg_guest_store`): a real store outside a trial; in a trial a
  journal entry. After the trial `check_store_journal` requires every journaled store to equal what the lifted body left in
  memory, and every watched cell the lifted body changed to have a journaled store of that value. A wrong value, a store
  the lifted body did not make, a missing store and more than 16 stores are mismatches. A QLEAF port may therefore store to
  guest memory; the plain replay rule "a port writes no guest memory" still holds for ELEAF and PLEAF rows.
- Limit: a class initializer that does not set its word on the first call could give a false mismatch (the port assumes it
  does); cells the hook did not name are not pre-state captured.

GC safepoints. `Regs::safepoint_n` / `smrpg_guest_safepoint_n` flush several registers at one safepoint (the lifted code
writes two at some of them) and set `c->pc`; in a trial they only imitate the context change, as `safepoint` does.

Selftest. A pre-state row's globals (flag bytes, metadata slots) are game memory, so the selftest writes fake values
(pointers into a private region) into those addresses for the row's iterations and restores the originals when the row
finishes (`restore_attack_globals`); the lifted body then runs its real lazy-initialization code against the fake graph with
stand-in callees. One call in sixteen has the flag clear, which exercises the deferral.

## Attack stat ports (task-55)

`Battle.BattleCharacterParam.get_calc_attack` (0x71021c3ea0) and `get_calc_magic_attack` (0x71021bc2e0) in
`decomp/smrpg_decomp/src/battle/attack.rs`, rows `QLEAF(main_0377 / main_0376, ..., smrpg_param_calc_attack, ..._pre)`. In
C#: `sum = _attack + _buff_mng._add_status[0]` (`_magic_attack` and index 2 for the magic method). A player character adds
`_weapon._base_value + Random.Range(_random_min, _random_max)` when `_weapon._is_exist` (physical only) and returns
`(int)((bonus + sum) * _buff_mng.attack_up_ratio())` (saturating cast as in the defence ports; +infinity gives
`int.MinValue`). Any other character returns `sum`, plus 5 per lit candle
(`BattleEventParam_StrawberryAdd.get_on_candle_num()`) when `BattleManager._instance._encounter_id` is 2076 (0x81c). Types:
`BattleCharacterWeapon`, `BattleManager`, `Il2CppClass`, and the three metadata slots by address (`attack::slots`). The body
spells out the compiled lazy initialization because the comparison sees every call: the method flag byte (0x7103cba000 +
0xc09 or 0xc0b), two `initialize_runtime_metadata` calls, the `BattleManager` class-initialized word (+0xe0) and
`il2cpp_runtime_class_init` (0x71000c4330), the static-field flag byte behind slot 0x7103a581d0 and its metadata call, and
the three safepoints (two registers flushed at the first two).

Shim `LiftedAttack` keeps the lifted function's locals (`x[0..31]`, sp) as the C does, writes exactly the listed registers
at each call site (the lists were read off the lifted C by a script; they are the `flush` module) and restores x19, x20,
x21, x29 and sp from the entry values at the return. Callees (traced `PLEAF`): `initialize_runtime_metadata` 0x71000c4150
and `il2cpp_runtime_class_init` 0x71000c4330 (main_0016), `Random.Range` 0x7101797f10 (main_0273), `attack_up_ratio`
0x71021c4070, `magic_attack_up_ratio` 0x71021c43d0 (main_0377), `get_on_candle_num` 0x7101bf7a10 (main_0320), the index
thrower 0x71000c4510. The hook snapshots the method flag, and for a non-player also the class word, the static-field flag
byte and the strawberry class word, and defers the call while the method flag is clear.

`Battle.BattleCharacterParam.calc_element` (0x71021be3c0, 21 instructions, `battle/element.rs`, `ELEAF`): the
`ElementManager._elements_state[kind]` state (1 to 4) selects one of four multipliers of a read-only table at 0x7102af4fa0,
any other state means 1.0, a kind past the array length throws. The damage code compares the result with 1.0 and 2.0.

## damage_calc (task-55): what is and is not ported

`SkillExecter.damage_calc` (0x71021ff580, 613 instructions) is not ported; it is partly covered by traced rows. Read from
the lifted C: about 90 call sites. 29 belong to the first-call `initialize_runtime_metadata` sequence, 8 are class
initializer checks with safepoints, 19 build debug log lines (`DebugMonitor.write`, `Int32.ToString`, `Single.ToString`,
`String.Concat`). The arithmetic is a handful of tests on the skill type and the element multiplier ending in
`(int)(num * ratio)`, with calls to `calc_renkei_ratio` 0x71021f5ed0, `set_acction_command_ratio` 0x71021f7ea0,
`calc_belome_barrior_ratio` 0x71021f5c50, `group_a_b` 0x7102201aa0, `calc_element` 0x71021be3c0, `is_barriered`
0x71021c3bb0 and `use_damage_guard` 0x71021be280. Why not now:

- The formula is the middle of `damage_calc`, not a function of its own, so it cannot be carved into a row.
- A bit-exact comparison needs every register at each of the 90 call sites to match, so the port would be a line-by-line
  transcription of the lifted C with a readable skeleton, roughly ten times the size of `get_calc_attack`.
- Its callees write through out-parameters and fields. The store journal now makes that comparable, but each is 100 to 390
  instructions of its own.

Done instead: the two attack stat methods, `calc_element`, the machinery above, and traced rows (lifted body unchanged,
live counts) for the callees listed. Next in order: `set_acction_command_ratio` (102 instructions, stores only, jump
table), `calc_belome_barrior_ratio`, `calc_renkei_ratio`, `group_a_b`, then the driver.

## damage_calc (task-56): three callees ported, the driver not

Done means for this task was the driver; it is not finished. What is built and compared:

- `Battle.SkillExecter.set_acction_command_ratio` (0x71021f7ea0, 102 lifted instructions, `QLEAF`), `Battle.BattleCharacterParam.use_damage_guard`
  (0x71021be280, `QLEAF`) and `is_barriered` (0x71021c3bb0, `LEAF`) in `decomp/smrpg_decomp/src/battle/damage.rs`, with IL2CPP names and field
  offsets (`SkillExecter` +0x18 `_cur_skill_result`, +0x20 `_acter`, +0x30 `_skill_obj`; `SkillResultOneSkill` +0x22 `_is_geno_hoarding_skill`,
  +0x50 `_action_command_succeed_ratio`, +0x54 `_action_command_sweetspot_ratio`, +0x58 `_hosei_ratio`, +0x5c `_is_renzoku_cmd_skill`;
  `BuffManager` +0x2c `_barrier_num`; `CharaSkillResultLogger` +0x49 `_is_use_barrior`). The compiled `set_acction_command_ratio` jumps through a
  129 byte table (read from the image, `id - 1` as an unsigned index; 23 entries differ from the default, the others and every index above 0x80 take it). The port spells the table as
  `CommandRule::of(skill_id)`: eight rules, every other id zeroes both ratios, an enemy (`_is_player == 0`) gets -0.5 and -1.0 (and -0.0 for id 727).
  The three methods flush no register, so the port leaves the register file alone.
- Out-parameter and store-through-pointer callees, the mechanism built. A callee that writes memory is never run twice in `ab` mode because a
  row is a replay row: the lifted body runs once on live memory and the port runs on a copy of the entry context with its stores journaled
  (`GuestStore`, `prestate::store_u8`, new `prestate::store_u32`). The pre hook snapshots every cell the port may write (`watch`), so a store the
  lifted body made and the port did not (or the other way round, or with another value) is a mismatch. This is also how an out parameter of a
  ported callee is compared: the callee's store to the caller's slot is a journaled store. Fix in `check_store_journal`: a cell the port stores
  twice (a default, then the real value; `_is_renzoku_cmd_skill` is cleared first and set later) now has to match memory at its last store only.
  Before, the earlier entry compared against the final memory and reported a false difference.
- Selftest graphs (`prepare_caller`, `prepare_battle` in `swap.c`): executer, acter, result record and skill object for `set_acction_command_ratio`
  (skill ids 0 to 0x8f in every run and the table edges 0x80 to 0x83, 0x2d6 to 0x2d8, 0, 1, 2 and the 32-bit extremes; `_is_player` 0, 1, 2, 0x80,
  0xff), a logger for `use_damage_guard`, and `_barrier_num` edge values for `is_barriered`: 25,625 calls each, 0 differences.
- Mutation results and live counts are in `backlog/tasks/task-56.md`.

Why the driver is not ported (measured from the lifted C of 0x71021ff580, 1,750 lines, 613 instructions). Its call sites, by callee: 29
`initialize_runtime_metadata` (the first-call lazy initialization behind the flag byte 0x7103cba000 + 0xd71), 19 `DebugMonitor.write`-shaped calls
(0x71018bebc0: x0 is `_monitor`, x1 a string literal from the metadata page) plus 4 string concatenations (0x71020147d0), 3 number-to-string
calls (0x710213a840) and 0x71018bebf0, 8 class-initializer checks (0x71000c4330) each followed by a safepoint, the small tests
(`calc_element`, `is_barriered`, `use_damage_guard`, `is_invincible` 0x71021bcb30, `Math.Max`), the four formula callees (`calc_renkei_ratio`, `set_acction_command_ratio`,
`calc_belome_barrior_ratio`, `group_a_b`) and the event-queue tail (0x71000c5060, 0x7101d7c0c0, 0x710015b4c0, 0x7101886a00,
0x7101d6a050: `System.Action` and `List<int>` calls with stores into the result logger). 21 distinct callees. Three things block a faithful
port in this task:

1. `REC_MAX` is 12 recorded callee calls per run; the first call alone makes 29 metadata calls before the formula, and the log lines add about 25 more. It would need to grow to about 48 and
   `Recording` (two `Ctx` per call) move off the stack.
2. The callees `calc_belome_barrior_ratio` (x2 = a float on the caller's stack, 341 lifted lines, a five way jump table, lazy initialization,
   a delegate loop), `group_a_b` (x1 = an int on the stack, 1,165 lines) and `calc_renkei_ratio` (757 lines, float math, a loop) are untraced
   by a port and write results to the caller's stack frame. The driver reads those slots after the call. A replay port runs second, so it
   would read the slot after later lifted code reused it. Design for a driver port, not built: a traced row may declare an out footprint
   (register holding the pointer, byte count); the recorder saves those bytes right after the call; `Regs::invoke_out` returns them in a trial
   (the callee never runs) and runs the callee and reads the slot in `on` mode. The port keeps its own stack locals in a shadow that the
   row compares with the recorded bytes at each call (the pre-call footprint) so a wrong value passed by reference is a mismatch.
3. Its selftest graph needs about ten classes (executer, two params with buff and element managers, skill object, result record, logger, three
   static-field pages) and a lazy-initialization model for 29 metadata calls, as large again as the port.

Still to do (task-57 built the recorder growth and the out footprint, and ported the first of the three big callees; see the next section for the remainder): the three big callees as rows (each lazy-initialization plus event-queue code, like `get_calc_attack`), the out
footprint and `REC_MAX` change above, then the driver. `is_invincible` and `calc_element` are ported already; the debug and string calls are
Unity natives that stay lifted (traced `PLEAF` rows, to be added when the driver is ported).

## damage_calc (task-57): recorder, out footprints, `calc_belome_barrior_ratio`

Task-56 listed three blockers for the `damage_calc` driver (0x71021ff580). Two are built here and one callee is ported; the driver, `calc_renkei_ratio` and `group_a_b` are not.

Recorder (`runtime/decomp/swap.c`). `REC_MAX` (12 calls on the stack) is gone. The recorded calls (target, the whole `Ctx` before and after, and an out footprint) live in a per-thread heap buffer that doubles from 64 (`REC_START`) up to 4,096 (`REC_LIMIT`) calls. A body that makes more traced calls than that stops the run (`decomp replay: more than 4096 traced calls in one body`, exit 10); the old "skipped, not compared" path is removed because it hid comparisons. The pre-state and store journal limit (`PRE_MAX`) went from 16 to 32 cells.

Out footprints (`OLEAF(chunk, addr, reg, off, bytes, in)` in `leaves.def`, a traced row like `PLEAF`). The callee writes `bytes` bytes (at most 16) at `x[reg] + off`: a slot in the caller's own stack frame (the driver passes `sp + 8` or `x29 - 4` to its formula callees and reads the slot back), or fields of an object the caller edits in place (a `List<int>` header). The recorder saves those bytes before the call (`out_pre`) and after it (`out_post`). The port keeps the area as a local buffer and calls `Regs::invoke_out(target, &mut area)` (`smrpg_guest_bl_out` in `swap.c`):

- Replay trial: the callee does not run. The call is matched as usual (same callee, same x0 to x30 and v0 to v31). With `in = 1` the port's bytes must equal `out_pre`, so a wrong value passed by reference, or a store the port made to the area earlier, is a mismatch there; then the buffer receives `out_post`. Journal entries and watch marks for cells inside the area are dropped, because the callee changed them after the port stored: they were just compared with the memory at the call instead of with the final memory.
- `on` mode: the bytes go to the area (`in = 1` rows), the callee runs on the live stack, the buffer receives what it wrote. A callee not declared with this footprint stops the run.

So no side-effecting callee runs twice on live memory in `ab` mode, and the out parameter is checked in both directions. Unit test: `invoke_out_returns_the_bytes_the_callee_wrote`. Footprints the driver will need (from its asm): `calc_belome_barrior_ratio` x2, 4 bytes, `in` (the driver stores 1.0f first); `calc_renkei_ratio` and `group_a_b` x1 (= `x29 - 4`), 4 bytes, `in`; `Single.ToString` (0x7101d6a050) x0 (= `sp + 8`), 4 bytes, `in`.

Frame (`decomp/smrpg_decomp/src/battle/frame.rs`). The lifted function's register locals as a struct, with `call`, `call_out` and `leave`: the port assigns `f.x[n]` where the compiled code does, and the three methods write the register lists the lifted code writes at the call or return (lists read off the lifted C by script: a register is written to the context only where it changed since the last write, `sp` only at some call sites). The epilogue restores x19 to x22, x29 and sp and never writes x30 back (the lifter artifact the earlier ports copy).

`Battle.SkillExecter.calc_belome_barrior_ratio(SkillObject, ref float)` (0x71021f5c50, 133 instructions, `QLEAF`, `battle/belome.rs`). If `_target._character_id` is 1609 (0x649) the method reads `GlobalWork.get_inst()._belomu_barrior_list` (a `bool[]`; a shorter array calls the index thrower) at an index chosen by the skill's element (a 5 entry byte table in the image: normal element only for calc type SP_ATTACK, jump unless `_acter._is_equip_kaiten_shoes`, fire, thunder, ice) and then by the skill id (id 1: entry 0; 0x9e and 0x9f, 0x2ba, 0x2c3 and 0x2c9: entry 4; otherwise the element result). When a barrier absorbs the hit it stores `ratio = 0`, then runs the inlined `List<int>.Clear()` and `Add(0)` once per old element on `_target._result_logger._multi_number`. IL2CPP lazy initialization (flag byte 0x7103cba000 + 0xd5d, four metadata slots), the `GlobalWork` class-initialized word and two back-edge safepoints are reproduced because the comparison sees them. Callees (all traced rows): `initialize_runtime_metadata`, the class initializer, `GlobalWork.get_inst` 0x7101886a00 (main_0285), the index thrower, `IsReferenceOrContainsReferences` 0x71000c5060 (main_0016), `Array.Clear` 0x7101d7c0c0 (main_0336) and `List<int>.Add` 0x710015b4c0 (main_0036, the one `OLEAF` row: 16 bytes at `x0 + 0x10`, `in`). The port carries the list header (`_items`, `_size`, `_version`) from `Add` call to `Add` call. Pre-state hook: flag byte (a clear flag defers the call), the class word, the list's items, size and version, the `ratio` cell.

Selftest graph (`prepare_caller` in `swap.c`): executer, acter, target, result logger, `List<int>`, skill object, a `GlobalWork` stand-in with a `bool[]` (lengths 0 to 9 and 0xffffffff), the class, a generic-context chain, and the flag byte plus three metadata slots in game memory, pointed into a private region and restored after the row. The stand-in `GlobalWork.get_inst` returns the fake object (`tl_stub_instance`); the other stand-in callees clobber registers as before. Element 0 to 7 and the 32-bit extremes, calc types 0 to 3, skill ids around 1, 0x9e, 0x9f, 0xa0, 0x2b9 to 0x2ca and the 32-bit extremes, list sizes 0 to 5 and -1.

What remains for the driver, in order (call-site breakdown of the driver is in the task-56 section above):

1. `calc_renkei_ratio` (0x71021f5ed0, 284 instructions): lazy initialization of 13 slots, then by skill id (0x2ba, 0x2bd, 0x2c3, 0x2c9, 0x2d2 and others up to 0x2d2) a debug write, `[x19] * 9 / 8`, `* 5 / 4`, `* 6 / 4` style integer scaling, and for 0x2c3 a float scaling (1.7f) plus a loop with `Random.Range(0, 21)`, float math (`fmul`, `fadd`, `fdiv`, saturating `fcvtzs`), `Math.Max`, a `List<int>` creation and `Add`, `Int32.ToString`, `String.Concat` and a 5 argument string format (0x7102023cb0). Needs `OLEAF(.., 1, 0, 4, 1)` for the `int&` and the float layer of `fp.rs`.
2. `group_a_b` (0x7102201aa0, 390 instructions): not decoded yet; it takes the same `int&` at `x29 - 4`.
3. The driver (613 instructions): its 29 metadata calls, 19 debug writes, 8 class initializer checks and the four formula callees. `calc_belome_barrior_ratio`, `set_acction_command_ratio` and the small tests are ported, so the driver port calls their Rust code directly (a traced call is recorded and matched; a ported callee that is reached through `smrpg_guest_bl` in `ab` mode stops the run, because it is not a traced row and not on the pure allowlist), with its snapshot hook naming the cells those ports read.

## damage_calc (task-58): `calc_renkei_ratio` ported, `group_a_b` and the driver not, and what made the build slow

`Battle.SkillExecter.calc_renkei_ratio(ref int damage)` (0x71021f5ed0, 284 instructions, `QLEAF`) is ported in `decomp/smrpg_decomp/src/battle/renkei.rs`
with IL2CPP names and field offsets (`SkillExecter` +0x10 `_monitor`; `BattleCharacterParam` +0x8c `_target_counter`, +0xb8 `_result_logger`;
`CharaSkillResultLogger` +0x2c `_number_state` (DAMAGE = 2), +0x50 `_multi_number`). It scales `damage` by skill id (0x2d2: x9/8, 0x2bd: x5/4,
0x2c9: x6/4, 0x2ba: x8/4, each a signed divide by 8 or 4 through `add bias; asr`) and for 0x2c3 multiplies by 1.7f (`+inf` becomes `int.MinValue`),
builds a `List<int>`, and for each of the target's hits adds `max((int)(((float)Random.Range(0, 21) * 0.01f + 0.9f) / 5f * (float)damage), 1)`.
All float work goes through `Regs::f32_arith`, `f32_div` (new), `f32_from_i32`, `i32_from_f32_trunc` and `f32_compare` (fp.rs, each checked against
the lifted helper in `ab` mode: 382,981 shadow checks, 0 differences in the final selftest run).

What the port needed that did not exist before:

- `Frame` (frame.rs) now carries vector registers (`v`, `set_s`, `call_v`, `call_out_v`, `leave_v8`: the epilogue reloads d8 and clears its upper half),
  writes x30 at every call, and `leave` restores x19 to x29. `Regs::v_high`, `Regs::f32_div`, `Regs::safepoint_nv` and `prestate::store_u64` are new;
  `swap.c` has `smrpg_guest_safepoint_nv` (the loop poll flushes x0, x8, x9, x22, v0, v1).
- New traced rows in `leaves.def` (every callee of a replay row must be a row): `OLEAF 0x710213a840` `Int32.ToString` (x0 = the int's address, 4 bytes,
  in: the slot of `damage` itself, `sp + 8` or `x29 - 4`), `OLEAF 0x7101b4fcc0` `List<int>..ctor` (x0 + 0x10, 16 bytes, out only: the port gets the header the
  constructor wrote), `PLEAF` `String.Concat` 0x71020147d0 and 0x7102023cb0, `Object.new` 0x71000c44d0 and the write barrier 0x71000c4090.
  `DebugMonitor.write` (0x71018bebc0), `writeLine` (0x71018bebf0) and `Math.Max` (0x710213e670) are ported `LEAF` rows on the pure allowlist, so the port calls them through the bridge.
- The pre-state hook snapshots `damage` (the lifted body stored the result before the port reads it), `_multi_number`, `_number_state` and the class
  initialized word. The port keeps `damage` in a local after its own stores. The class word is read from the snapshot on the first loop pass and live afterwards,
  because the first pass may call the initializer.
- Selftest graph in `swap.c` (`prepare_caller`, case 0x71021f5ed0): executer, target, result logger, skill object, damage cell (0 to 0x7fffffff, negatives, float-looking bit
  patterns), 0 to 6 hits and the 32-bit extremes, a stand-in `List<int>` that `Object.new` returns, a class cell, the flag byte and the 13 metadata slots (restored after the row).
  The caller stack of the selftest grew from 16 to 64 words: this function's frame is 0x70 bytes and the old buffer overflowed into the `leaves[]` table (the symptom was
  "callee differs" with a huge difference count; `swap.c` now names the callee and the row).
- `swap.c` `ON` mode counts calls of replay rows and prints `decomp on addr=... calls=...` at exit, so an `on` run shows the route reached them.

Result: selftest 25,625 calls, 0 differences, 0 live differences, the A/B battle route and the task42 route 0 `decomp mismatch`. Two mutations are caught (`backlog/tasks/task-58.md`).

Why the build was slow (task-57: 15 min; task-58: measured). Three separate causes, none of them in `wrap_chunk.py`:

1. `flags.make` order. CMake writes one `Custom options` line per wrapped chunk into the target's `flags.make`, in the order of the `chunks` list, and every
   object of the target depends on `flags.make`. Rows are read hand-written first, so the first hand-written row for a chunk that only had generated rows
   moved that chunk forward, rewrote `flags.make` and rebuilt all 400+ objects. Task-57's four new chunks did this; so did task-58's first row set (a build killed
   after 11 minutes). Fix: `list(SORT chunks)` in `game.cmake`. The set of chunks is fixed (every chunk has generated rows), so a new row in an existing chunk no longer changes `flags.make`.
2. `gen_game/prelude.h` and `ops_gen.h`. Every lifted and wrapped chunk includes them (the depfile lists them and `runtime/rt.h`, `ir_ops.h`, `ir_simd_a.h`,
   `ir_simd_b.h`, `mode_lift.h`). `tools/lifter/gen.py` and `build.py` rewrite them unconditionally, so any lifter `gen` or `build` run (not this task: they were
   rewritten at 12:41 today by another run) touches both and the next `build_game.py` recompiles everything (a full rebuild with no source change: 7 min 57 s,
   load 60 percent). Not fixed here (tools/ is not in this task's scope): the writers should rewrite only when the content changes.
3. One wrapped chunk costs minutes. A new row in an existing chunk recompiles that 1.5 to 2.6 MB lifted C file at `-O2`: measured 225 s and 577 s for the same
   single-chunk change (main_0262 plus `swap.c`) on a machine at 60 percent CPU load from other programs. A Rust-only change builds in 28 s. Rows therefore go in chunks that
   have to change anyway, and one batch of rows per build.

Measured builds: no-change baseline after another run touched `prelude.h` 7 min 57 s; first row set (flags order bug) killed at 11 min; first build with the sort fix
11 min 27 s (one time: `flags.make` changed order once); `swap.c`-only 1 to 2 min; Rust-only 28 s; one row in one existing chunk 225 s to 577 s.

What remains for the driver, in order (the first item is done):

1. ~~`calc_renkei_ratio`~~ (this task).
2. `group_a_b` (0x7102201aa0, 390 instructions, lazy init flag 0x7103cba000 + 0xd73 and 8 slots, the skill type `_calc_type == 0xd` branch, `damage / 2` through
   `cinc; asr`, an enumerator loop (0x7101b52040 fills a 24 byte struct at sp, then 0x71018fe040 and 0x7100183d20 per element, a counter of the hits of `0x71021bc4b0`; the callee names are not confirmed), `Int32.ToString` and
   `String.Concat` debug lines). Callees with no row yet: 0x71021bc4c0, 0x71021bc4b0 (`BattleCharacterParam` predicates), 0x71021be020, 0x7101b52040, 0x71018fe030, 0x71018fe040,
   0x7100183d20, 0x71000ba3b0, 0x7101d6a050, 0x710153a0f0, 0x710153a100, 0x710153a110, 0x710003e500. The struct 0x7101b52040 writes is on the caller's stack: an `OLEAF` with x1 and 24 bytes
   (past the 16 byte `OUT_MAX`, so raise it). It needs the same register-exact `Frame` port as renkei.
3. The driver (613 instructions): its 29 metadata calls, 19 debug writes, 8 class initializer checks. `calc_belome_barrior_ratio`, `calc_renkei_ratio` and `set_acction_command_ratio` are
   ported and the driver port calls their Rust code directly (a traced call is recorded and matched; a callee reached through `smrpg_guest_bl` that is neither a traced row nor on the pure
   allowlist stops the run). Footprints it needs: belome x2 4 bytes in; renkei and `group_a_b` x1 (= `x29 - 4`) 4 bytes in; `Single.ToString` x0 (= sp + 8) 4 bytes in.

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
