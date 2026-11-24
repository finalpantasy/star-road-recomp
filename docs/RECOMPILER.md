# Star Road Recomp: recompiler and runtime architecture (task-4)

Status: design for stage 1 (AOT static recompilation ARM64 to x86-64) and the stage 2 function-swap path. Date 2026-11-24. Owner of this file: task-4.
Inputs: `docs/analysis/il2cpp.md` (task-2), `docs/analysis/platform-survey.md` (task-3), `docs/analysis/prior-art.md` (task-9), `docs/analysis/re-tools.md` plus the shared AI RE tools note Parts 2.8 and 2.9 (task-8), `docs/analysis/known-bugs-1.0.1.md` (task-7). Reference recomps were read for ideas only; no code was copied.
Spike code: `tools/spike/` (our own). Spike output (generated C, objects, logs, JSON): `A:\AI\Recomps\smrpg-work\spike\`, never in Git.

## 1. Decisions in one table

| Area | Decision | Measured or sourced reason | Section |
|---|---|---|---|
| Lifter output | C, one C function per guest function, compiled by clang 23 (LLVM-MinGW, x86-64-v3, -O2) | Spike: emitting LLVM IR directly saves nothing (C front end is about 0 to 5 percent of compile time); C keeps stage 2 swaps and debugging simple | 3.3, 13 |
| Lifter structure | Decoder, then one small IR with executable semantics, then passes, then a C emitter. The same IR drives a C interpreter used as fallback and as a test oracle | Kirikae pattern (one source of truth for semantics); the spike's direct decoder-to-C path is what we avoid | 3.2 |
| Register model | Guest registers are C locals; written registers are flushed to the context only where the AAPCS64 makes them visible (call arguments, callee-saved, results), with per-callee checks | Spike: 25 percent less run time than registers-in-memory on 841 real leaf functions; equal end state on all 841 | 3.4, 13 |
| Guest memory | Identity mapping: guest address equals host address. The guest image and all guest regions live at fixed host addresses we reserve at startup | Spike: masked base-plus-offset costs 15 percent on leaf functions; identity mapping works on Windows at 0x7100000000 | 4.1 |
| Indirect calls | Dense per-module table indexed by (address - text base) / 4; miss goes to the interpreter and the profile | Spike: 1.3 ns per lookup over all 157,221 functions cold, binary search 104 ns | 3.5 |
| Exceptions | Unwind by return: `__cxa_throw` sets a pending exception, lifted code tests it after each call and jumps to landing pads built from the guest LSDA tables | Only 1,929 functions have an LSDA (task-2); the public port treats every throw as fatal and hit bugs from it | 4.4 |
| GC visibility | Cooperative stop at safepoints (every HLE call, svc, and loop back-edge poll) with a full register flush; `nn::os::SuspendThread` and `GetThreadContext` are HLE | The embedded Boehm GC stops threads through those two `nn::os` imports (strings in `main`) | 4.5 |
| HLE boundary | Lift `main` whole. HLE the `nn::` API surface that `main` imports (987 symbols), NVN (535 procs), and `nn::movie` (31 symbols). Lift pure SDK code (libc, libm, libc++) | Enumerable, closed import lists from task-3; IPC-level emulation kept only as a fallback | 4.6 |
| Graphics | NVN HLE on Vulkan with a Maxwell shader translator, all variants prepared before play | Shaders exist only as Maxwell binaries (task-3); a Unity GfxDevice-level path would need 416 shaders rewritten before the first frame | 5 |
| Upgrades | Options registry; every option has a faithful value; faithful mode is one switch that restores every faithful value | D-004 | 6 |
| Stage 2 | Per-function swap through generated typed adapters; original kept as `f_X__orig`; per-function runtime switch for A/B and dual-run checks | Proven idea (public port replaces functions one at a time); we add typed adapters and A/B in one binary | 7 |
| Build | Our own Python build driver for generated code (content-hashed chunks of about 400 functions, parallel clang at BelowNormal, object cache on A:); CMake for the hand-written runtime | Spike: 400 functions per file costs about 10 CPU seconds per 1,000 functions, 1 per file costs 330 | 9 |
| Stage 2 language (D-003) | Owner's decision. Recommendation: C | Generated code is C; swaps need no binding layer; one compiler | 7.4 |

## 2. What we are building and the constraints

- Input: the owner's v1.0.1 dump (`main` 28.4 MB NSO, 36.8 MB `.text`, 157,221 functions; `sdk`; `subsdk0` = `nn::movie`; RomFS 6.82 GB). Facts from task-2 and task-3.
- Output: a Windows x64 executable plus a game-code library built on the owner's machine from the owner's dump. Nothing from the game enters Git or a release.
- Quality bar: correct against the original first, measured by reference tests (section 8). Speed and upgrades never override a faithful mode.
- Machine rules that shape the design: heavy processes one at a time, BelowNormal, under 10 minutes; no visible windows during tests.

Pipeline:

```
dump (A:) --task-1--> exefs, romfs
exefs/main --task-2--> functions.tsv, metadata TSVs, LSDA and FDE tables, main_v101.elf
          --lifter--> decode -> IR -> passes (boundaries, jump tables, liveness, callee summaries, EH sites)
                      -> C chunks (A:\...\gen\) -> clang (cached objects) -> game library
runtime (repo, hand-written C): loader, memory map, threads, HLE nn::, NVN on Vulkan, audio, movies, options, tests
stage 2 (repo or private, D-005): decompiled functions -> typed adapters -> swap manifest -> relink
```

## 3. Lifter

### 3.1 Function boundaries

- Source of truth: `functions.tsv` from task-2 (157,221 functions: 152,217 from eh_frame FDEs, 5,004 from gap splitting; class and confidence tier per row).
- Checks the lifter runs on every build, each a hard error with a report:
  1. Every `bl` target, every IL2CPP method pointer, invoker, adjustor thunk, generic method pointer and vtable slot (from the registrations at 0x33c6828 and 0x33c7820) must be a function start.
  2. A `b` or conditional branch that leaves its function must land on a function start (it becomes a tail call, 3.6). If it lands mid-function, the boundary is wrong: split the target function there (secondary entry point, below) and log it.
  3. Every byte of a function must decode, except known literal pools.
- Secondary entry points: a function entered in the middle (a split boundary, or a target found only at run time) gets an extra C entry `f_X_at_Y` that jumps to the label. Generated, not hand-written.
- Compiler-outlined helpers (`OUTLINED_FUNCTION_*` style, non-standard register use) are detected by the callee summary pass (3.4) and get the conservative calling rule.
- Tier D boundaries (2,014 functions, task-2) are lifted like the others; the interpreter fallback (3.5) and the boundary checks above catch errors at build or run time.

### 3.2 IR with executable semantics

Decoder (A64 base, FP, NEON, exclusives, system) produces IR ops: register read and write, NZCV as four separate values, typed arithmetic, memory access with size, signedness and ordering, call, tail call, branch, indirect branch, svc, sysreg. Each op has one semantic definition written in C in `runtime/ir_ops.h` (task-5). That one definition is used three ways:

1. The C emitter prints calls to those inline functions (or their expansion), so the lifted code and the semantics cannot drift.
2. A small C interpreter executes the same ops for code we did not lift (fallback, 3.5).
3. Oracle tests compare the ops against an external CPU (section 8, L1).

The spike skipped the IR and lowered straight from decode to C strings (`tools/spike/emit_c.py`). That was enough to measure the language and register model, and it showed the problem: two emitters (context and locals) already disagreed in their register write-back until the ABI-visible state was defined. One IR avoids that class of drift.

Rejected: lifting through an existing decompiler IR (Ghidra P-code or angr VEX). Both are lossy for flags and FP details and add a heavy dependency to every build.

### 3.3 Output language: C

Chosen from the AC2 spike (section 13): C through clang 23.

- LLVM IR written directly: the proxy (clang -O0 front end to `.ll`, then `.ll` to object at -O2) took the same optimizer time as C (best of three runs: 42.2 CPU s versus 46.3 CPU s for the same 3,857 functions, inside the run-to-run spread); the C front end is small next to the optimizer. IR output is also 10 times larger on disk (203 MB of `.ll` for 17 MB of C) and harder to read when debugging.
- Cranelift (Kirikae's backend): not measured. It needs crates downloaded, which this task may not do. Kirikae reports "well under a minute" per game but weaker optimization than clang. Our extrapolated full -O2 build is about 2 to 3 minutes on 16 jobs (section 9), so build speed does not justify a second backend. Reopen if full builds exceed 10 minutes.
- Rust or Zig source output: rejected for stage 1 output. Compile time per function is higher than C for generated code of this shape, and stage 2 swaps would need a binding layer between generated C-ABI functions and the decomp.

Every lifted function: `void f_<guestaddr>(Ctx *restrict c)`. Generated files hold about 400 functions in address order, so a change to one function rebuilds one file (LASRecomp's fixed chunk bounds idea).

### 3.4 Register model and calling convention

- Inside a function, guest registers are C locals (`x0` ... `x30`, `sp`, four flag bytes, `vNl` and `vNh` halves). clang allocates them to host registers.
- At a call (`bl`, `blr`, tail call), only registers a callee may read are written back to the context: x0 to x8, x16 to x30, sp, v0 to v7, the low halves of v8 to v15. At a return: x0, x1, x19 to x29, sp, v0 to v7, the low halves of v8 to v15. At svc, safepoints and unresolved indirect jumps: everything written.
- After a call, only caller-saved registers are reloaded; x19 to x29, sp and the low halves of v8 to v15 keep their local values. This is AAPCS64 trust, and it is also what makes exceptions cheap: at a landing pad the locals already hold the values the unwinder would restore (4.4).
- Callee summaries: a pass computes, per function, which registers it reads before writing and which callee-saved registers it fails to restore. A function that breaks the AAPCS64 (outlined helpers, hand-written assembly) is marked non-standard; calls to it use full flush and full reload. Indirect calls always use the standard rule, since vtable and delegate targets are compiled C++ methods; a debug build checks this at run time (callee-saved registers compared before and after each indirect call).
- Measured in the spike: naive locals (flush everything written at every call) produced 52 percent more x86 code than registers-in-memory (3.84 MB versus 2.52 MB `.text`); the AAPCS64 rule cut that to 23 percent more (3.11 MB). Liveness-based flushing (task-5) removes stores of registers that are dead at the call and should close most of the rest.
- Rejected: registers kept only in the context struct (the naive lifter). 25 percent slower on the spike's leaf functions (sum of per-call times 866 ns versus 692 ns over 841 functions), and it cannot give correct callee-saved values at landing pads without extra saves.
- Rejected for stage 1, kept for later: passing x0 to x7 as real C arguments and returning x0 and x1 in registers. It speeds call-heavy code further but couples every caller to callee summaries; revisit after the first full profile.

### 3.5 Indirect branches and the fallback

- `blr xN` becomes `rt_icall(c, target)`: look up `g_fn[(target - text_base) >> 2]` in a dense per-module table of function pointers (4-byte offsets from the library base, 37 MB virtual for `main`). Spike: 1.34 ns per lookup with random targets over all 157,221 functions, 0.70 ns over a hot set of 512; binary search over the sorted list was 104 ns and 46 ns.
- Miss (address not a known function start): call the IR interpreter for that address, log the address to `profile.tsv` with a hit count and caller, and keep running. The next build adds the address as a function (secondary entry or new function). Idea from Kirikae's interpreter fallback; we keep it as a safety net, not the plan of record, because our inventory is already complete by construction (eh_frame plus registrations).
- IL2CPP specifics: vtable calls, interface dispatch (`unresolvedVirtualCallPointers`, 428), delegates and the 7,212 runtime-filled method slots of full generic sharing all end in `blr` to an address that is a registered method pointer or a shared generic body. All of them are in the table; the runtime-filled slots only change which known address is loaded. Test: the boot and route runs report zero fallback hits for these classes; any hit is a boundary bug.
- Invokers (7,798) and adjustor thunks (9,331) are lifted like any function at first. Because they are data-driven stubs (task-2, tier A), a later pass may generate them from metadata instead; that is an optimization, not a correctness need.

### 3.6 Tail calls

- `b` to another function start becomes `__attribute__((musttail)) return f_T(c);`. Spike: clang 23 on the mingw target accepted `musttail` in all 3,857 functions and emitted `jmp` (226 tail jumps and 272 calls counted in one chunk), so tail-call chains never grow the host stack.
- Conditional branches out of the function become the same inside an `if`.
- Falling off the end of a function (code after a no-return call, or a split boundary) becomes a tail call to the next address.
- `br xN` (indirect jump) is first matched as a jump table (3.7). If it is not one, it becomes `c->pc = target; musttail return rt_dispatch(c);`, which resolves the target through the same table and the fallback.

### 3.7 Jump tables

- Pattern match the clang AArch64 switch idiom: bound check (`cmp` plus `b.hi` or `b.hs` to default), table address (`adrp` plus `add`), index load (`ldrb`, `ldrh` or `ldrsw` with shift), base (`adr`), `add` with shift, `br`. Read the table from `.rodata` at build time, bound from the compare.
- Emit a C `switch` on the index with `goto` to labels. Out-of-table targets are an error.
- Test: every `br` in `main` is classified (jump table, tail call through register, or unknown) in a build report; unknown must be zero before M1, and each unknown has a recorded reason.
- Fallback: a function with an unresolved `br` gets a resume switch over all its block labels at entry, reached through `rt_dispatch`, so a jump to any block start still works; any other address goes to the interpreter.

### 3.8 Calls and returns

- `bl` sets x30 to the guest return address (constant) before the call, because guest code saves x30 to its stack and the frame chain must look real to the guest's own stack walkers and to our crash reports.
- `ret` becomes a C return. Structured call and return is assumed. A debug build checks that x30 at `ret` equals the address set by the matching call and logs any mismatch (longjmp-style code would show here).

### 3.9 FP and NEON exactness

- Scalar FP: C `float` and `double` with helpers for every place where x86 and AArch64 differ: NaN propagation and default NaN (FPCR.DN), FMIN and FMAX and the NM forms, signed zero rules, float-to-int conversion saturation (C casts are undefined out of range), FRINT rounding modes, FCMP flags.
- FPCR modes: read the game's FPCR writes at run time; FZ (flush-to-zero) maps to MXCSR FTZ and DAZ per thread; rounding mode maps to MXCSR RC. Only the modes the game sets are supported; any other write is logged.
- FMA: x86-64-v3 has FMA3, so fused multiply-add matches. The v2 fallback build uses software fma (exact, slower). Same two-build split as the public port.
- Reciprocal and square-root estimates (FRECPE, FRSQRTE, FRECPS, FRSQRTS): implemented bit-exact from the architecture's tables, never with x86 `rcpps` (different results).
- NEON: one helper per op (SSE4.1 or AVX2 inside), each covered by the L1 oracle test. Unity.Mathematics and Burst code (9,336 methods) is the main NEON user; it decides which ops come first. The spike's subset failed on 118 of 4,000 sampled functions because of NEON, 3 percent of functions but 20 percent of bytes (large Unity native functions).

### 3.10 Atomics and memory ordering

- AArch64 is weakly ordered; x86 is TSO. Mapping: LDAR to an acquire load (plain `mov`), STLR to a seq_cst store (`xchg`, needed because STLR followed by LDAR must not reorder), DMB ISH and DSB to `mfence`, DMB ISHLD and ISHST to a compiler barrier only.
- Exclusives: LDXR records address and value in the context; STXR becomes a compare-and-swap against the recorded value (`lock cmpxchg`). Difference from real LL/SC: an ABA write (value changed and changed back) succeeds here and fails on hardware. It is harmless for counters, spin locks and the CAS loops compilers emit; a test suite of the game's lock-free paths (Boehm mark stack, IL2CPP atomics, Unity job queues) runs under stress to confirm. Fallback if a real ABA case appears: a per-address sequence counter in a side table for that call site.
- Plain guest loads and stores: the C compiler may hoist a plain load out of a spin loop that the guest relies on (`while (flag == 0)` with no barrier). Spike: compiling every guest access as a relaxed atomic cost nothing measurable (0.89 of plain, within noise, same end state on 841 functions). Decision: relaxed atomic accesses for all guest memory by default; plain accesses only where a pass proves the address is thread-local (stack-relative). This removes one class of hangs; the public port spent versions 13 to 16 of its notes getting past a lost wakeup in the condvars.
- Test: litmus tests (message passing, store buffering, LDAXR/STLXR counter under 32 threads) built from lifted test functions; a 1-hour stress run of the Horizon sync primitives (4.2) with zero lost wakeups.

### 3.11 Patches and hooks

- Patch table keyed by guest address with the expected original instruction word; a mismatch fails the build (LASRecomp idea, prevents patching the wrong version). Patches are rare in our design: most changes are function swaps (section 7).

## 4. Runtime model

### 4.1 Guest memory map

- Identity mapping: we choose the guest layout, so we place every guest region at a host address that is free in a Windows x64 process and reserve it before anything else allocates. Guest pointers are host pointers; no base add or mask in lifted code.
- Layout (guest equals host address): `main` at 0x7100000000 (the base our relocated ELF already uses; `.text`, `.rodata`, `.data`, `.bss` = 81.5 MB), `sdk` and `subsdk0` above it; heap and alias region of 48 GB (the size the public port needed for `AddressRegionManager`) at 0x1000000000; stacks and TLS regions in their own reserved range. The exact numbers are reported by our svcGetInfo HLE, so the game asks us where its regions are.
- Reservation: `VirtualAlloc2` placeholders at startup for the whole guest range, then commit on map. The host executable is linked at a low image base; the host heap and DLLs never land in the guest range.
- Faults: guest null or wild pointers hit unmapped pages; a vectored exception handler turns them into a crash report with guest address, function name from `functions.tsv`, guest frame chain and the last HLE calls. IL2CPP uses explicit null checks, so no fault is part of normal game behavior (verify in M1: zero faults on the boot route).
- Measured: identity mapping at 0x7100000000 worked in the spike bench (VirtualAlloc at the exact address); the masked alternative was 15 percent slower.
- Rejected: Kirikae's reserve-the-39-bit-space-and-mask design. Safer against wild host writes, but costs a mask and add per access, and the guard it gives (no host corruption) is already given by the fact that host data never sits in the guest range.
- Fallback: if a Windows build or anti-cheat style DLL injection ever occupies our range, the same lifted code builds with `MEM_MASKED=1` (the spike already supports it) at the measured cost.

### 4.2 Threads and synchronization

- Each guest thread is a host thread with its own `Ctx`, a 64 MB host stack reserve (deep lifted recursion; the public port's compiler threads also needed 64 MB) and a guest stack in guest memory of the size the game requests (main thread 16 MB per `main.npdm`).
- Priorities: guest priority 44 for the main thread (npdm); a mapping table from Horizon priorities to Windows priorities. Core affinity masks are ignored except for the test mode.
- Horizon sync: `svcArbitrateLock`, `svcArbitrateUnlock`, `svcWaitProcessWideKeyAtomic`, `svcSignalProcessWideKey`, `svcWaitForAddress`, `svcSignalToAddress` implemented on `WaitOnAddress` and per-key wait queues with the exact Horizon semantics (owner tag bits, waiter bit, signal counts). These carry every mutex and condvar in nn::os, libc++ and Unity.
- Test: a sync test suite (lifted SDK mutex and condvar code driven by many threads) plus the 1-hour stress run in 3.10.
- Unity's 30 or so threads (MainThread, UnityGfxDeviceWorker, Job.Worker, Background Job.Worker 0 to 15, GC Finalizer, and others, from the public port's notes) are created by the game through nn::os; nothing special per thread.
- Timing bugs from host scheduling: the public port had to patch a frame-job race that a Switch never shows (their `delegate_null.c` notes: a host thread preempted between two adjacent guest stores). Our answer is the same as theirs only as a last resort; first we look for a missing barrier in our mapping, because the guest code is correct on hardware.

### 4.3 TLS and system registers

- TPIDRRO_EL0 points to a per-thread Thread Local Region (0x200 bytes) in guest memory that our thread HLE creates, laid out as Horizon does (IPC buffer, nn::os thread pointer, TLS slots). TPIDR_EL0 is the user-writable value. Both are fields of `Ctx`, read and written by `mrs` and `msr`.
- CNTVCT_EL0 returns a 19.2 MHz tick derived from the host clock (test mode: derived from the frame counter, 8.2). CNTFRQ_EL0 is 19,200,000. DCZID_EL0 and CTR_EL0 return the Switch values.

### 4.4 Exceptions (IL2CPP managed exceptions are C++ exceptions)

Facts (task-2): 1,929 functions carry an LSDA (BCL 1,065, game 393, libil2cpp 371, CRI 81, glue 18, libc++ 1); the personality routine and unwinder are imported from `nnSdk.nss`. The public port does not support this at all: its `__cxa_throw` HLE logs and stops the game, and three of its issues (41, 60, 155) were exceptions the game catches on the Switch.

Approach, unwind by return:

1. `__cxa_allocate_exception`, `__cxa_throw`, `__cxa_rethrow`, `__cxa_begin_catch`, `__cxa_end_catch`, `__cxa_get_exception_ptr`, `_Unwind_Resume` and `__gxx_personality_v0` are HLE. `__cxa_throw` stores the exception object and its `type_info` in `c->exc` and returns.
2. After every call in a function, lifted code tests `c->exc` (one load and a predicted-not-taken branch). Spike: this added 5 percent to `.text` and no measurable compile time.
3. If the call site lies in an LSDA call-site range with a landing pad, the lifter emits the action-table match at build time: the catch clauses' `type_info` addresses are constants, so the test is a short compare chain (`catch (Il2CppExceptionWrapper&)`, `catch (...)`, cleanup). On a match it sets x0 to the exception pointer and x1 to the selector, clears `c->exc` as the personality would, and jumps to the landing pad label. Callee-saved registers are already correct in locals (3.4).
4. With no landing pad, the function returns at once; its caller repeats step 2.
5. Cleanup pads end in `_Unwind_Resume`, whose HLE sets `c->exc` again, so propagation continues.
6. If `c->exc` reaches a thread entry or an HLE-to-guest callback boundary, the runtime reports an uncaught exception with the type name and the guest frames (same report the public port prints, but only for truly uncaught throws).

Two-phase semantics: C++ searches for a handler before unwinding. IL2CPP always has a catch at `il2cpp::vm::Runtime::Invoke` and at thread entry, so a missing handler means a bug, and we report it at the boundary. If a case appears where the game relies on terminate-before-unwind, the fallback is a phase-1 search over the static LSDA tables along the host return path (we know each frame's call site from the return chain), done before step 4.

Test: L2 tests that throw from lifted IL2CPP code and check the managed `catch` and `finally` run (for example `int.Parse` on bad input, `Dictionary` key-not-found caught by the game, the 393 game functions with LSDAs exercised on the routes); boot route must report zero uncaught exceptions.

Rejected:
- Host C++ exceptions or SEH thrown from the HLE through lifted C: C has no catch; it would require C++ output or `__try` (MSVC-only), and the landing pads still need guest LSDA semantics.
- setjmp at every try region: about 20 to 50 ns per region entry on hot IL2CPP paths and it forces locals to memory.
- Lifting the SDK unwinder and personality: they walk real AArch64 frames with DWARF CFI and restore registers into a machine context that does not exist in lifted code.

### 4.5 GC visibility (Boehm, conservative)

Facts: the binary embeds Boehm (`GC_mark_some`, `GC_init_gcj_malloc`, `GC_try_to_collect_inner`); it stops the world through `nn::os::SuspendThread` and `nn::os::GetThreadContext(ThreadContextInfoAarch64*)` (both imported by `main`); `boot.config` sets `gc-max-time-slice=3`, so incremental collection is on.

Roots Boehm scans and how each stays correct:
- Static data: `.data` and `.bss` are guest memory. Nothing to do.
- Thread stacks: guest stacks are guest memory; lifted code spills to them exactly as the original (stack slots are lifted memory ops). Our thread HLE reports guest stack bounds.
- Registers of stopped threads: `SuspendThread` HLE sets a request flag; the target thread stops at its next safepoint, flushes every written register to `Ctx`, and parks. `GetThreadContext` returns `Ctx`. Safepoints: every HLE call and svc (blocked threads are already parked there) and a poll on loop back-edges in functions that have loops.
- Registers of the collecting thread: Boehm pushes its own callee-saved registers through ordinary guest code; with the flush rule at calls the context holds the right values.
- HLE code never keeps a guest object pointer only in host memory across a call that can allocate.

Measured: the back-edge poll cost nothing measurable at run time on the leaf set (0.89 of no-poll, within noise) and added 22 percent to `.text` (4.05 MB versus 3.32 MB) because the spike flushes everything at each poll; flushing only live registers at polls (task-5) shrinks it.

Test: a GC stress scenario (allocate managed objects on worker threads while the main thread forces `GC.Collect`) with a checker that every live object survives; heap verification mode (`GC_DUMP_REGULARLY`-style counts) compared against the same scenario in the reference.

Fallback: if polls cost too much in a hot loop, the poll moves to the loop's exit plus a bounded iteration count, or the loop's function is marked GC-unsafe and the GC waits for it (it ends quickly in practice). If incremental collection needs page-protection dirty bits rather than IL2CPP's write barriers (verify in M1), implement the protection HLE on guest pages with `VirtualProtect` and a fault handler.

### 4.6 Horizon HLE boundary

| Layer | Treatment | Why |
|---|---|---|
| `main` (Unity, IL2CPP, game, CRIWARE, glue) | Lifted, all 157,221 functions | The thing we recompile |
| `sdk` libc, libm, libc++, nn::util, pure code | Lifted | Exact behavior, no reimplementation risk |
| `sdk` nn:: services used by `main` (987 imports: hid 124, os 100, friends 75, audio 51, fs 43, ec 39, account 26, socket 25, swkbd 24, and others) | HLE at the C++ function level | Closed list from task-3; avoids emulating IPC |
| `sdk` memcpy, memset, strlen and other hand-tuned routines | HLE with host versions | Faster and avoids non-AAPCS assembly |
| `subsdk0` (`nn::movie`, 31 imports) | HLE with a VP9 and Opus decoder (license-compatible library, owner yes needed for the install) | Android stagefright stack is not worth lifting |
| NVN (`nvnBootstrapLoader` then 535 procs by name) | HLE on Vulkan (section 5) | |
| SVCs (from lifted SDK code) | HLE kernel: memory, threads, sync, events, time, info | Needed by the lifted SDK parts |
| IPC (`svcSendSyncRequest`) | Only as fallback for an nn:: function we cannot HLE cleanly: lift its SDK code and serve the IPC command | Keeps the door open without paying for it now |

Coverage report (Kirikae idea): the runtime logs each HLE function, SVC and NVN proc used with implemented, stub or missing status and a backtrace on first use of a stub. This sizes HLE work from the first boot.

Stubs return documented safe values (friends offline, eShop unavailable, network down) and are listed in one table so faithful mode behavior is visible.

## 5. Graphics

Decision: NVN HLE on Vulkan, with a Maxwell shader translator (Maxwell SASS to SPIR-V through our own shader IR), all shader variants found in the dump translated and turned into pipelines at first run (cache on A: or the user's data folder).

Why not the Unity GfxDevice level in stage 1: shaders exist only as NVN Maxwell binaries with reflection, no source (task-3 fact 1); a GfxDevice-level renderer must translate those same binaries or rewrite 416 shaders (240 Shader Graph) by hand before the first frame, and the GfxDevice code is unnamed Unity native code (tier B and C in task-2). NVN is a closed, enumerable surface: 535 procs looked up by name, about 277 asked for (public port).

Stage 2 path: the decompiled engine renders natively with our own shaders; the stage 1 NVN path is the reference, and its translated shaders are the specification for the rewrite.

Known pitfalls checked by tests from day one (all from prior art, task-9): RGBA4 channel order; `nvnDeviceSetDepthMode` near = 0 (zero-to-one depth); separate texture and sampler handles OR-ed at `c[2][0x168 + 8r]` and `c[2][0x568 + 8r]` for bindless `TEX.B`; `VOTE.VTG` early exit in vertex shaders; the post chain's unwritten last row and column (kept in faithful mode, fixed as an option); ASTC 6x6 block-linear textures (deswizzle, then BC7 or GPU ASTC).

Test: (1) NVN shadow mode (Kirikae idea): a host model of every NVN object and command, checked call by call against invariants and dumped per draw; (2) per-draw capture of selected frames compared against reference captures (section 8, L4); (3) a sweep of all 416 Addressables scenes with 0 skipped draws (the public port's criterion) and 0 untranslated shader instructions.

Fallback: for a shader the translator cannot handle, a hand-written SPIR-V replacement keyed by shader hash; for an NVN feature with no Vulkan equivalent, emulate it in the HLE (for example conservative raster calls the game makes without enabling it).

## 6. Upgrades behind options, faithful mode

Every upgrade is an entry in one options registry: id, type, default, faithful value, the swaps or hooks it enables, and its test. "Faithful mode" sets every option to its faithful value at once and is what every reference test runs in. The options file is plain text in the user's data folder.

| Upgrade | Mechanism | Faithful value | Test |
|---|---|---|---|
| Resolution up to 4K and render scale | NVN HLE scales the window and the render targets Unity creates; `Screen.SetResolution` clamp swapped (public port found the 1920x1080 clamp); the UI stays in its 1920x1080 panel and scales | 1920x1080 docked, 1280x720 handheld | Faithful captures match reference; 4K capture downsampled to 1080p matches faithful within an SSIM bound |
| High frame rate (120, 144, 240, uncapped) | Present interval and `Application.targetFrameRate` or `QualitySettings.vSyncCount` through the internal-call table; field logic is time-based (public port measured); battle logic steps once per frame, so `BattleManager.update` is swapped for a fixed 60 Hz accumulator (0 to 3 steps per frame, debt cap) with input held per step; battle animation interpolated later | 60 fps, present interval 1 | Battle step counter = 60 per second within 1 at every rate; same battle state hash at logic frame N at 60 and at 240 fps with the same per-step input; per-frame logic audit list from a profile run (functions called once per frame that update counters) all classified |
| Ultrawide (low priority) | Camera aspect plus the 16:9 places listed by the public port (title wipe, fades, menu background, movie rectangles) | 16:9 with bars | Captures at 21:9 have no unfilled bars outside the faithful list |
| Quality of life | Function swaps chosen by the owner (text speed, skips); each is its own option | Off | Each has a scenario test |
| Bug fixes from task-7 | Swaps; speedrun glitches keep the original behavior in faithful mode | Original behavior | Scenario reproducing the bug, before and after |
| Mods: loose files | RomFS HLE overlay: `mods/<name>/romfs/...` wins over the dump in a fixed priority order; Addressables bundles and `data_tbl` replaceable; optional HD texture folder keyed by texture hash, with a dump tool | No overlay | Overlay test with a known replaced file; hash of the merged RomFS listing in faithful mode equals the dump's |

## 7. Stage 2: the function-swap mechanism

### 7.1 Mechanics

- A swap manifest (TSV, generated or edited, in the repo or private per D-005) lists guest addresses with their replacement C function names.
- For a swapped address the generator renames the lifted body to `f_X__orig` and emits `f_X` as a small dispatcher: `if (g_swap[id]) replacement_adapter(c); else f_X__orig(c);`. Every direct call, tail call and table entry already targets `f_X`, so internal calls reach the replacement too (the public port reaches the same goal by linking an overrides file first with `-FORCE:MULTIPLE`; we avoid duplicate-symbol tricks and keep both versions callable).
- Only the chunks holding swapped functions are regenerated; everything else comes from the object cache. Relink takes seconds.
- `g_swap[id]` is a runtime switch per function (options file or debug console), so A/B comparison needs no rebuild.

### 7.2 Typed adapters

The adapter for a swapped IL2CPP method is generated from metadata (parameter types, `this`, `MethodInfo*` last argument, value-type returns in x8, floats in v registers), so decompiled code is plain typed C:

```c
/* generated */
void adapt_BattleManager_update(Ctx *c) {
    BattleManager_update((BattleManager *)G(c->x[0]), (const MethodInfo *)G(c->x[1]));
}
/* decompiled, written by us */
void BattleManager_update(BattleManager *self, const MethodInfo *method) { ... }
```

Calls from decompiled code to functions not yet decompiled go through generated typed stubs that load a scratch `Ctx`, call `f_Y` and return the typed result. Struct layouts come from the task-2 export (`ghidra_structs.tsv`, box header already subtracted). Non-IL2CPP functions (Unity native) get adapters from a hand-written signature table.

### 7.3 Verification of a swapped function

1. Recorded-call replay: in record mode the runtime captures, for a chosen function, the entry `Ctx`, the bytes of every guest page the original reads (guard pages trap first touches) and the exit state. A replay harness runs the replacement on that snapshot and compares the ABI-visible end state and written memory. Hundreds of calls per function from a route.
2. Dual run: with `g_swap[id]` toggled, the same recorded route must give the same per-frame semantic state hashes (8.2).
3. A function is "behavior verified" when both pass; the per-function status table (task-8 recommendation: undiscovered, lifted, named and typed, decompiled, compiles, behavior verified, integrated) records it with evidence links. Byte matching is not a goal for IL2CPP code.

### 7.4 Stage 2 language (D-003, owner's decision)

Recommendation: C. Reasons from this design: the generated side is C, adapters and stubs are generated C, one compiler builds both, and a swapped function is an ordinary C function the debugger shows next to its lifted original. Rust would work through `extern "C"` and generated bindings, at the cost of a second toolchain and an FFI layer at every swap; its safety gains are smaller here because decompiled code operates on raw guest memory by design. The spike numbers do not decide D-003 (they measure the generated side).

## 8. Test strategy

### 8.1 Levels

| Level | What | Oracle | Pass rule |
|---|---|---|---|
| L0 decoder | Every instruction word in `main` `.text` decoded, compared with `llvm-objdump` (mnemonic class, branch targets, immediates) | llvm-objdump | 0 mismatches; spike smoke check in 13.6 |
| L1 instruction semantics | Each IR op on random register and memory states (edge values weighted: 0, -1, INT_MIN, NaNs, denormals, infinities) | (a) Unicorn AArch64 locally (venv install needs the owner's yes, Part 2.9 item 4); (b) native ARM64 execution of the same instruction encodings on a GitHub ARM64 runner (random encodings only, no game bytes; owner yes needed for CI) | 0 mismatches over 10,000 cases per op; FP compared bit for bit |
| L2 function differential | Lifted function versus the oracle CPU running the original bytes on the same start state (the spike's bench harness plus Unicorn) and versus the IR interpreter | Unicorn, interpreter | Equal ABI-visible end state and memory; spike already checks 5 build variants against each other on 841 functions |
| L3 boot milestones | sdk constructors (112) and game constructors (288), `nnMain`, RomFS mount, Unity threads, `level0` loaded, first frame presented (counts from the public port) | Counts and logs | Each milestone reached; 0 fallback hits, 0 uncaught exceptions |
| L4 scenario | Recorded input routes with per-frame semantic state hashes and captures | Reference run of the original (8.3) | Hash streams equal; captures within bound |
| L5 soak and stress | Sync and GC stress, 1-hour play loops, memory growth | Invariants | No hangs, no leaks over a bound |

### 8.2 Determinism and semantic state hashes

- Test mode: the guest clock advances by exactly one frame per present; input is injected per frame from the route file; audio runs on a null device at a fixed rate.
- Semantic state hash: a list of game state fields located through metadata (static fields of managers such as the battle and field managers, party stats, flags, RNG state, Mario position), read after each frame and hashed. Allocation addresses and thread timing do not enter the hash, so worker-thread scheduling does not break equality while game logic on the main thread stays deterministic.
- Fields are chosen per subsystem when its reference test is written (AGENTS rule: every subsystem gets a reference test before it is called done).

### 8.3 The reference "original"

Needed for L4: something that runs the original game and exposes state per frame. Options, in order of preference: (1) an instrumented Switch emulator build (state read at frame boundaries through the same metadata field list; deterministic input injection); (2) the owner's Switch with a capture device for visual references only. Both need the owner's decision and installs. Until then L4 compares stage 1 against itself across builds (regression) and against published facts.

### 8.4 Known-bug and prior-art regressions

The task-7 bug list and the public port's issue list become scenario tests: each faithful-mode test reproduces the original behavior; each fix option has a test that it is fixed.

## 9. Build system and budgets

- Generated code: our own Python build driver (`tools/build/`, task-5). Inputs: dump hashes, lifter version, options that change code. Steps: lift (parallel by chunk), write C chunks of about 400 functions with a content hash each, compile changed chunks with clang at BelowNormal priority with N jobs, link the game library with lld. Object cache keyed by chunk hash on A:. Each run is resumable and stops cleanly at a time cap, so a full build can be split into runs under 10 minutes.
- Runtime and host code: CMake (4.5.1 is on A:) with the MinGW Makefiles generator (mingw32-make ships with LLVM-MinGW); Ninja preferred if the owner approves the install.
- Release model (later): the installer builds the game library on the user's machine from the user's dump (public port and LASRecomp do this), with a bundled clang. Generated code never ships.

Budgets (owner's machine, 16 jobs; extrapolated from the spike, section 13.4):

| Item | Budget | Spike-based estimate |
|---|---|---|
| Lift all of `main` (9.0 M instructions) | under 60 s | about 55 s single-process Python (decode 3.1 us and emit 3.2 us per instruction), under 10 s split over 16 processes |
| Full -O2 compile of `main` | under 5 min | 2 to 3 min wall (about 2,000 to 2,500 CPU s), before NEON-heavy functions; with a 1.5x margin for them, 3 to 4.5 min |
| Incremental rebuild after one swap | under 30 s | one 400-function chunk (about 4 s) plus relink |
| Game library `.text` | under 150 MB | 120 to 170 MB at the spike's 3.5x to 4.6x expansion; liveness-based flushing should bring it near the lower end |
| Dispatch table | 37 MB virtual | 4-byte entries over 9.2 M slots |

Comparison: our own New Pokemon Snap recomp compiled 62,961 functions in 179 s clean with clang 23 x64-v3; the public SMRPG port builds the game code on the user's PC inside its installer (generator plus a slimmed-down zig toolchain).

## 10. Ideas taken from other recomps, and what we do differently

| Idea | Source | What we do differently |
|---|---|---|
| One C function per guest function, address-to-function table for indirect calls, frozen generator run on the user's machine | LASRecomp, SMRPG public port (MIT) | Dense table measured (1.3 ns); registers as C locals with AAPCS64 rules instead of context-only; IR with shared semantics |
| Patches check the original word first; hooks via a runtime call | LASRecomp | Swaps are the main mechanism; patches are rare |
| Overrides linked first so they win even for internal calls | SMRPG public port | Generator renames the original to `f_X__orig` and adds a runtime switch, so A/B needs no rebuild |
| Chunk files at fixed address bounds, cached objects | LASRecomp | Content-hashed chunks of about 400 functions (measured sweet spot) |
| Shader variants prepared before play | LASRecomp, public port, New Pokemon Snap | Same, plus a shadow-mode NVN model |
| One IR with executable semantics, interpreter fallback with a profile, coverage report, NVN shadow mode | Kirikae (MIT or Apache-2.0) | C backend instead of Cranelift; fallback is a safety net, not the discovery method |
| Debug aids: per-function call counters, first-caller trace, memory watch, hung-thread dump, guest-address crash log | Public port | Built into the runtime from M1 |
| Fixed-step battle logic for high frame rates, held input per step | Public port | Verified by a battle state hash equal at 60 and 240 fps, not only a step counter |
| Lifted function boundaries from analysis, not heuristics | N64Recomp, XenonRecomp (MIT) | Boundaries from eh_frame plus IL2CPP registrations, checked every build |
| Guest registers in a context struct with optional local caching | XenonRecomp | Locals by default; flush rules derived from the ABI and callee summaries |
| Owner-local generation cache keyed by input hashes | New Pokemon Snap recomp (ours) | Same idea per chunk |

What nobody we read does, and we do: C++ exceptions across lifted code (4.4), a GC safepoint model (4.5), relaxed-atomic guest accesses by default (3.10), reference tests against the original with semantic state hashes (8), and the typed stage 2 swap path (7).

## 11. Rejected designs and why

| Rejected | Why |
|---|---|
| LLVM IR as lifter output | Same optimizer time as C in the spike, 10x larger files, worse debugging, no help for stage 2 |
| Cranelift backend | Not measurable without downloads; weaker optimization; build speed is already inside budget with clang |
| Registers only in the context struct | 25 percent slower in the spike; no correct callee-saved values at landing pads |
| Naive locals (flush everything at each call) | 52 percent more code than the context model in the spike; the AAPCS64 rule keeps the speed with less code |
| Masked base-plus-offset guest memory | 15 percent slower in the spike; identity mapping is possible because we choose the guest layout |
| Binary search dispatch | 104 ns per lookup versus 1.3 ns |
| One function per C file | 330 CPU s per 1,000 functions versus 10 at 400 per file (per-file overhead dominates) |
| Host C++ exceptions, SEH or setjmp per try region for guest exceptions | Section 4.4 |
| Fatal `__cxa_throw` (public port) | The game catches exceptions in normal play |
| Unity GfxDevice-level renderer in stage 1 | No shader source; would need 416 shaders rewritten before the first frame |
| Full IPC-level Horizon emulation (emulator or Kirikae style) | Large surface for no gain while `main`'s nn:: imports are a closed list; kept as a fallback |
| Lifting `subsdk0` (Android stagefright movie stack) | 10,886 exports of codec plumbing; HLE of 31 functions is smaller |
| Plain non-atomic guest accesses everywhere | Compiler may hoist spin-loop loads; relaxed atomics measured at no cost |
| Pausing threads with host SuspendThread and reading host registers | Guest registers live in host registers and spills chosen by clang; no mapping back exists |
| Copying any recomp's code | Owner rule D-006 |

## 12. AC1: hard parts, chosen approach, test, fallback

| Hard part | Chosen approach | Test | Fallback |
|---|---|---|---|
| IL2CPP managed exceptions (C++ throw and catch across lifted code) | Unwind by return: pending exception in `Ctx`, check after each call, landing pads and catch matching emitted from LSDA tables at build time, HLE `__cxa_*` and `_Unwind_Resume` (4.4) | L2 throw and catch tests incl. `finally` and rethrow; 0 uncaught exceptions on boot and routes | Phase-1 search over static LSDA tables along the return path; setjmp only at HLE-to-guest callback boundaries |
| Boehm GC scanning stacks and registers | Guest stacks in guest memory; cooperative SuspendThread at safepoints (HLE, svc, loop back-edge polls) with full flush; GetThreadContext returns `Ctx` (4.5) | GC stress scenario with live-object checker; heap counts versus reference | Poll moved to loop exits with iteration bound; page-protection dirty bits if incremental mode needs them |
| Function boundaries | task-2 inventory plus per-build checks of call targets and registration pointers; secondary entries for mid-function targets (3.1) | Build report: 0 mid-function targets unexplained; L3 0 fallback hits | IR interpreter on unknown addresses with profile feedback into the next build |
| Indirect calls, vtables, interface dispatch, generic sharing (7,212 runtime slots) | Dense address table (1.3 ns measured), all registered method pointers and shared bodies in it (3.5) | Route runs report 0 fallback hits for IL2CPP classes | Interpreter fallback and profile |
| Jump tables | Pattern match clang's switch idiom, emit C `switch` (3.7) | Build report: every `br` classified, 0 unknown | Per-function resume switch over block labels |
| Tail calls | `musttail` (verified on clang 23 mingw: lowers to `jmp`) (3.6) | Spike compile of 3,857 functions; deep tail-chain test | Trampoline loop in `rt_dispatch` |
| Register model and ABI trust | C locals, AAPCS64 flush and reload, callee summaries mark non-standard functions (3.4) | Debug build checks callee-saved registers around indirect calls; spike: equal end state on 841 functions across models | Full flush and reload for any callee not proven standard |
| FP exactness | Helpers for NaN, min and max, conversions, rounding, FCMP; FPCR to MXCSR; FMA3; bit-exact estimate tables (3.9) | L1 against Unicorn and native ARM64, bit for bit | Software FP helpers for any op that still differs |
| NEON | One helper per op on SSE4.1 or AVX2, prioritized by use in Unity.Mathematics and Burst (3.9) | L1 per op | Scalar reference implementation of the op |
| Atomics and memory ordering | LDAR acquire, STLR seq_cst, DMB to mfence or compiler barrier, exclusives as CAS, relaxed atomics for all guest accesses (3.10) | Litmus tests, 32-thread exclusive counter, 1-hour sync stress | Side-table sequence counter for an ABA-sensitive site; seq_cst for a flagged access |
| Horizon threads and sync | Host threads, Horizon arbiter and condvar semantics on WaitOnAddress (4.2) | Sync test suite and stress run, 0 lost wakeups | Single global kernel lock for the arbiter (slower, simpler) |
| Guest memory map | Identity mapping with reserved placeholders (4.1) | Spike: mapped at 0x7100000000; L3 boot | `MEM_MASKED=1` build (spike-supported, 15 percent slower) |
| TLS | TLR per thread in guest memory, TPIDRRO and TPIDR in `Ctx` (4.3) | nn::os TLS slot tests on many threads | none needed |
| NVN and Maxwell shaders | NVN HLE on Vulkan, Maxwell to SPIR-V translator, variants prepared at first run (5) | Shadow mode, per-draw captures, all-scenes sweep with 0 skipped draws | Hand-written SPIR-V per shader hash; HLE emulation of missing NVN features |
| Movies and audio middleware | `nn::movie` HLE with VP9 and Opus decoders; CRI ADX2 runs lifted, audio out HLE (4.6) | Movie frame hashes, audio sample hash in test mode | Decode the USM ourselves outside `nn::movie` |
| Frame-rate assumptions (battle per-frame step) | Fixed-step swap of `BattleManager.update`, per-frame logic audit (6) | Step counter 60/s and equal battle state hash at 60 and 240 fps | Lock the battle scene to 60 fps while the field runs high |
| Resolution and 16:9 assumptions | Render-target scaling in NVN HLE, clamp swap, UI panel kept (6) | Faithful capture equality, 4K SSIM bound | Render at 4K and keep the UI layer at 1080p |
| Host timing races the Switch never shows | Correct ordering mapping first; relaxed atomics; then a targeted swap with a written cause (4.2) | Stress and route runs | Targeted swap, as the public port did |
| Reference correctness against the original | Semantic state hashes per frame on recorded routes, reference from an instrumented emulator or captures (8) | L4 | Self-regression across builds until the reference exists |
| Build time and size | Python driver, 400-function chunks, cache, time-capped resumable runs (9) | Build report against budgets | -O1 for cold chunks (measured 0.88x the -O2 compile time) |
| Stage 2 function swap | Rename to `f_X__orig`, dispatcher with runtime switch, typed adapters, replay and dual-run checks (7) | Replay equality and route hash equality per swapped function | Link-time override only (no runtime switch) |

## 13. AC2 spike: lifter output language and build system

### 13.1 Question

Which lifter output and build system: C through clang, LLVM IR through clang, or another backend; and how should generated code be split and built. Secondary: register model, memory mapping and dispatch, since they change the numbers.

### 13.2 Method

- `tools/spike/a64.py`: our own decoder for the A64 integer, branch, load and store, exclusive, system and scalar FP subset, plus vector move and zero.
- `tools/spike/emit_c.py`: lowers to C in four variants: `ctx` (registers in memory), `local_naive` (locals, flush all at each call), `local` (locals, AAPCS64 flush and reload), `local_exc` (`local` plus an exception check after every call), `local_poll` (`local` plus GC safepoint polls on back-edges).
- `tools/spike/lift.py`: samples 4,000 functions uniformly at random (seed 20261124) from `functions.tsv`, reads their bytes from `main_v101.elf`, lifts, writes 100-function chunks, declarations and stubs for missing callees under `A:\AI\Recomps\smrpg-work\spike\lift\`.
- `tools/spike/build_time.py`: compiles each variant with clang 23.1.4 (LLVM-MinGW 20261020, `-march=x86-64-v3 -O2`), 16 parallel jobs, BelowNormal priority; LLVM IR proxy; chunk-size experiment. Three full runs.
- `tools/spike/bench.py` and `bench_rt.c`: identity-maps the relocated image at 0x7100000000, runs every lifted leaf function (no calls, no svc) from a fixed start state with a vectored-exception fault catcher and a watchdog, hashes the ABI-visible end state, times 5,000 calls x 7 repeats x 3 interleaved process runs per configuration, minimum per function; also times dispatch lookups. Runs hidden (no window), BelowNormal, pinned to one core.
- `tools/spike/decode_check.py`: decoder cross-check against llvm-objdump.

Machine: the owner's PC (32 logical CPUs), other work running at the same time, so timings carry noise (13.5).

### 13.3 Coverage of the subset

| Class | Sampled | Lifted | Bytes sampled | Bytes lifted |
|---|---|---|---|---|
| unity_native | 1,402 | 1,287 | 479,692 | 322,064 |
| unity_bcl_csharp | 1,229 | 1,217 | 217,248 | 206,296 |
| il2cpp_glue | 529 | 526 | 36,988 | 36,752 |
| game_csharp | 438 | 430 | 108,296 | 104,136 |
| cri_native | 168 | 163 | 25,944 | 23,576 |
| thirdparty_c | 108 | 108 | 4,904 | 4,904 |
| libil2cpp | 52 | 52 | 11,196 | 11,196 |
| cri_csharp | 45 | 45 | 6,024 | 6,024 |
| nn_sdk | 20 | 20 | 320 | 320 |
| libc_libcxx | 9 | 9 | 1,552 | 1,552 |
| Total | 4,000 | 3,857 (96.4 percent) | 892,164 | 716,820 (80.3 percent) |

Failures: NEON 118, LSE-style or other load/store forms 15, other FP 8, other 2. 179,205 instructions lifted. 947 lifted functions are leaves; 841 of them ran to completion from the synthetic start state (106 fault on zero-filled pointers, as expected).

### 13.4 Numbers

Lift (Python 3.14, one process): decode 0.55 s for 179,205 instructions (3.1 us each); emit 0.48 to 0.68 s per variant (2.7 to 3.8 us each).

Compile, 3,857 functions, 716,820 guest bytes, 16 jobs (CPU s is the sum of per-file clang times; run 1 is the first ABI-rule run, runs 2 and 3 are repeats):

| Configuration | CPU s, runs 1 / 2 / 3 | Best CPU s per 1,000 functions | x86 `.text` bytes | Notes |
|---|---|---|---|---|
| `ctx` -O2 | 42.2 / 34.7 / 45.5 | 9.0 | 2,523,217 | registers in memory |
| `local_naive` -O2 | 64.3 / 49.5 / 72.2 | 12.8 | 3,844,831 | flush all written registers at calls |
| `local` -O2 | 52.0 / 42.3 / 56.7 | 11.0 | 3,106,159 | AAPCS64 flush rule |
| `local_exc` -O2 | 49.2 / 46.3 / 82.4 | 12.0 | 3,318,139 | plus exception check after calls |
| `local_poll` -O2 | 64.9 / 70.5 / 137.5 | 16.8 | 4,049,370 | plus GC polls, full flush at each poll |
| `local_exc` -O1 | 43.5 / 40.9 / 54.1 | 10.6 | 3,429,148 | |
| `local_exc` -O0 | 28.7 / 23.4 / 37.0 | 6.1 | 19,035,548 | |
| LLVM IR proxy: C to `.ll` at -O0 (front end only) | 27.4 / 25.2 / 64.2 | | | 203 MB of `.ll` |
| LLVM IR proxy: `.ll` to object -O2 | 51.7 / 42.2 / 87.0 | 11.0 | 3,429,915 | optimizer and codegen only |
| `local_exc` -O2, 1 function per file (300 functions) | 98.8 / 94.4 / 322.9 | 314.6 | | per-file overhead |
| `local_exc` -O2, about 400 functions per file (10 files) | 38.7 / 40.4 / 63.6 | 10.0 | 3,318,446 | |

Run 3 ran while other work loaded the machine (every row slower by 1.3x to 3x), so the best of three is used for decisions. The `.text` sizes are identical across runs (deterministic output).

Extrapolation to all of `main` (36.18 MB of code bytes, about 50.5 times the lifted 716,820 bytes): `local_exc` at 400 per file, best run, is 38.7 CPU s x 50.5 = about 1,950 CPU s, so about 2 minutes wall on 16 jobs; a busy machine (run 3) gives about 3,200 CPU s, about 3.5 minutes. NEON-heavy functions that the spike could not lift are larger than average, hence the 1.5x margin in section 9.

Run-time bench, 841 leaf functions, same start state, minimum over 21 timings each (sum of per-call ns after subtracting the 0.96 ns loop overhead; geometric mean ratio over the 44 functions at 2 ns or more):

| Configuration | Sum ns | Median ns | Geomean vs `local` | End-state hash equal to `local` |
|---|---|---|---|---|
| `local` (identity memory, plain access) | 692.5 | 0.385 | 1.000 | 841 / 841 |
| `ctx` (registers in memory) | 866.3 | 0.405 | 1.065 | 841 / 841 |
| `local` with masked memory | 806.8 | 0.465 | 1.149 | 841 / 841 |
| `local` with relaxed atomic accesses | 646.1 | 0.330 | 0.890 | 841 / 841 |
| `local_poll` | 748.9 | 0.450 | 0.894 | 841 / 841 |

Dispatch lookup (4 M random lookups): over all 157,221 function addresses, binary search 104.0 ns, dense table 1.34 ns; over a hot set of 512, 45.6 ns and 0.70 ns.

### 13.5 Reading the numbers

- C versus LLVM IR: equal optimizer cost (CPU s within run-to-run noise), so the front end is not where time goes. Decision: C.
- Chunk size: per-file overhead is large (clang start, header parse); 1 function per file is 30 times more CPU than 400 per file. Decision: about 400 functions per file, which still gives about 390 files for `main`, plenty for 16 to 32 jobs.
- Optimization level: -O1 is about 0.88x the -O2 CPU time with 3 percent more code; not worth a split now. -O0 is cheap to build but 5.7 times larger code; useful only for a fast debug build.
- Register model: locals with the AAPCS64 rule are faster than registers in memory and bigger than them; naive locals are both bigger and no faster. The size gap is the next target (liveness).
- Memory: masked access is a measurable cost; relaxed atomics and back-edge polls are not measurable on this set (their ratios below 1.0 are noise; leaf functions here run 0.3 to 10 ns, where alignment and frequency effects dominate).
- Limits: leaf functions only, synthetic inputs, a busy machine. Call-heavy code, which the flush rules affect most, is not timed here; task-5 times it on real call chains once enough of `main` lifts. Correctness of the spike's FP helpers is not established (no oracle yet); only cross-variant agreement is.

### 13.6 Decoder smoke check

`tools/spike/decode_check.py` decoded 300 more randomly chosen functions (seed 7, functions up to 4 KB) with our decoder and with `llvm-objdump`: 14,780 instructions compared (mnemonic class, branch targets, ADRP and ADR values), 0 mismatches. The first run reported 217 mismatches; all were a parsing bug in the checker (it read the bit number of `tbz` as the target), fixed and rerun. This is a smoke check of the spike decoder only; the L0 test covers every word of `main` and every field.

### 13.7 Decision (AC2)

Lifter output: C, one function per guest function, 400 functions per file, clang 23 `-O2 -march=x86-64-v3` (plus a v2 build later), registers as C locals with AAPCS64 flush rules, identity-mapped guest memory, relaxed atomic guest accesses, dense dispatch table. Build: our own Python driver with content-hashed chunks and an object cache for generated code; CMake for the runtime. Expected full `main` build: 3 to 4.5 minutes wall on 16 jobs, under the 10-minute rule.

## 14. Open items for the coordinator

1. Reference original for L4 (8.3): instrumented emulator and or the owner's Switch with capture. Owner decision and installs.
2. Oracle installs: Unicorn venv (Part 2.9 item 4); optional GitHub ARM64 runner for native instruction tests (random encodings only, no game bytes).
3. Movie decoders for `nn::movie` HLE: choose license-compatible VP9 and Opus libraries; install needs a yes.
4. Ninja for the runtime build: optional; MinGW Makefiles work without it.
5. D-003 stays open; recommendation C (7.4).
6. Verify in M1: incremental GC write-barrier mode, FPCR values the game sets, zero guest faults on boot, 0 fallback hits.
