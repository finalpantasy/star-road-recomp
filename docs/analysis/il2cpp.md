# IL2CPP metadata recovery and function inventory (task-2)

Status: AC1 to AC4 pass. Date: 2026-11-24. Inputs are read only; every generated file lives on A: and none enters Git.

## Result in short

- All 31 metadata tables parse, and the CodeRegistration and MetadataRegistration are located without symbols. The method pointer table has 67,746 slots, equal to the 67,746 metadata methods.
- 63,227 methods map to a code address. The other 4,519 have no code by design (abstract, open generic definitions, methods of open generic types).
- All 157,221 `.text` functions carry an origin class. 96,299 are tier A or B (exact or string evidence), 58,908 are tier C (between two same-class anchors, leave-one-out accuracy 99.977 percent), 2,014 are tier D (weak). None stayed unknown.
- 20 of 20 spot-checked methods disassemble cleanly with `llvm-objdump` and match what they should do.

## Inputs

| Input | Path | SHA-256 |
|---|---|---|
| NSO `main` (v1.0.1) | `A:\AI\Recomps\smrpg-work\v101\exefs\main` | `7da4288368b14543473092ed5f30fc9fe47aad0d30ccf87894961f6889c78c6e` |
| `global-metadata.dat` (v29) | `A:\AI\Recomps\smrpg-work\v101\romfs\Data\Managed\Metadata\global-metadata.dat` | `adee2f91e08df2f73e7fd85de68673d275d8a7b837c801072695c0238d1ede98` |

NSO segments after LZ4 decode (all three per-segment SHA-256 values in the header match):

| Segment | Start RVA | Size |
|---|---|---|
| .text | 0x0 | 36,847,096 |
| .rodata | 0x2324000 | 15,345,188 |
| .data | 0x31c7000 | 9,248,912 |
| image incl. .bss | | 81,502,208 |

## Commands (run from the repo root, outputs go to `A:\AI\Recomps\smrpg-work\analysis\il2cpp\`)

```
python -I tools/il2cpp/nso.py                 # segment map, hashes, dynamic section
python -I tools/il2cpp/metadata.py            # table counts
python -I tools/il2cpp/export_tables.py       # TSVs and registrations.json (AC1, AC2)
python -I tools/il2cpp/build_features.py      # function boundaries, string and call features
python -I tools/il2cpp/classify.py            # origin classes, Ghidra name files (AC4)
python -I tools/il2cpp/loo_check.py           # leave-one-out check of tier C
python -I tools/il2cpp/ghidra_export.py       # C# struct layouts
python -I tools/il2cpp/nso2elf.py             # main_v101.elf for objdump and Ghidra
python -I tools/il2cpp/spot_check.py          # AC3 (needs llvm-objdump on PATH)
python -I tools/il2cpp/crosscheck_dumper.py   # diff against Il2CppDumper output (see Cross-check)
```

Code: `lz4block.py` (own LZ4 block decoder), `nso.py`, `metadata.py`, `registration.py`, `export_tables.py`, `ehframe.py`, `functions.py`, `build_features.py`, `classify.py`, `loo_check.py`, `ghidra_export.py`, `ghidra_apply_il2cpp.py`, `nso2elf.py`, `spot_check.py`. plus `crosscheck_dumper.py`. All written for this project; no code from Il2CppDumper or Cpp2IL was read or copied. Both tools were run only as external cross-checks, after our own map was finished.

## AC1: metadata tables (v29)

Row counts, read straight from the table headers and cross-checked against the TSV row counts:

| Table | Rows | | Table | Rows |
|---|---|---|---|---|
| typeDefinitions | 8,045 | | parameters | 72,953 |
| methods | 67,746 | | properties | 12,411 |
| fields | 40,480 | | events | 104 |
| stringLiteral | 11,189 | | fieldDefaultValues | 10,812 |
| images | 59 | | parameterDefaultValues | 1,668 |
| assemblies | 59 | | genericContainers | 1,022 |
| genericParameters | 1,198 | | genericParameterConstraints | 468 |
| interfaces | 3,456 | | interfaceOffsets | 8,582 |
| vtableMethods | 81,191 | | nestedTypes | 3,085 |
| attributeDataRange | 22,083 | | exportedTypeDefinitions | 1,937 |
| fieldMarshaledSizes | 3,570 | | fieldRefs | 287 |
| referencedAssemblies | 337 | | unresolvedVirtualCallParameterTypes | 1,569 |

TSVs written: `images.tsv` 59 rows, `types.tsv` 8,045, `methods.tsv` 67,746, `fields.tsv` 40,480, `strings.tsv` 11,189, `generic_insts.tsv` 3,044, `generic_classes.tsv` 5,315, `generic_methods.tsv` 14,926.

Largest images by method count (all 59 are in `images.tsv`): Assembly-CSharp 15,086, mscorlib 9,662, Unity.Mathematics 9,336, UIElementsModule 5,352, CoreModule 4,961, Burst 3,628.

## AC2: registrations and method pointers

Found with no symbols in the binary:

- CodeRegistration at RVA 0x33c6828. Method: find the string `Assembly-CSharp.dll` in rodata, then the CodeGenModule that points at it, then the module pointer array, then the CodeRegistration field that points at the array. The count before it is 59 modules, equal to the 59 images.
- MetadataRegistration at RVA 0x33c7820. Method: search `.data` for two adjacent (count, pointer) pairs with count 8,045 (fieldOffsets, typeDefinitionsSizes) and step back to the structure start.

Registration contents:

| Table | Count |
|---|---|
| codeGenModules | 59 |
| genericMethodPointers (also genericMethodTable rows) | 3,172 |
| invokerPointers | 7,746 |
| unresolvedVirtualCallPointers | 428 |
| interopData | 551 |
| reversePInvokeWrappers | 13 |
| types (Il2CppType) | 22,263 |
| methodSpecs | 14,926 |
| genericClasses | 5,315 |
| genericInsts | 3,044 |
| fieldOffsets / typeDefinitionsSizes | 8,045 each |

Method pointer mapping: `module(image).methodPointers[(method.token & 0xFFFFFF) - 1]`, with the invoker from `module.invokerIndices` at the same index. Sum of module method slots is 67,746, equal to the metadata method count (AC2 pass). Every method has an invoker.

Where the 67,746 methods go:

| Group | Methods |
|---|---|
| Has a code address | 63,227 |
| Abstract | 1,336 |
| Generic method definitions (open) | 641 |
| Non-abstract methods on open generic types | 2,541 (includes 68 runtime-implemented delegate stubs) |
| Other (no pointer, no explanation) | 1 |
| Total | 67,746 |

Generic method instances: of 14,926 method specs, 3,171 have their own pointer through the genericMethodTable. The other 11,755 have none. 11,535 of them belong to a method definition that does have a pointer-bearing instance, and 2,225 of the pointer-bearing specs use `__Il2CppFullySharedGenericType` arguments, so the build uses full generic sharing and those specs run the shared body (see the generic sharing risk below). The remaining 220 are specs over open type parameters (`T`, `TValueType`), not real instantiations.

## AC3: 20 spot checks

`llvm-objdump -d` on `main_v101.elf` (our relocated ELF, base 0x7100000000). Each row: 4-aligned start that equals a function start, every instruction decodes, a terminator exists. Six rows are semantic probes with an expected instruction pattern. Result: 20 of 20 plausible. Full data: `spot_check.tsv`.

| Method | RVA | Bytes | First instruction | Check |
|---|---|---|---|---|
| System.Object::.ctor() | 0x1deb8e0 | 4 | ret | probe: returns at once |
| System.String::get_Length() | 0x2029c90 | 8 | ldr w0, [x0, #0x10] | probe: length at +0x10 |
| System.String::get_Chars(int) | 0x2018e60 | 64 | stp x29, x30, [sp, #-0x20]! | probe: loads a char |
| UnityEngine.Mathf::Abs(float) | 0x2058880 | 96 | str d8, [sp, #-0x30]! | probe: contains `fabs` |
| UnityEngine.Mathf::Max(int,int) | 0x2058a80 | 12 | cmp w0, w1 | probe: compare and select |
| System.Math::Abs(int) | 0x213e210 | 96 | stp x29, x30, [sp, #-0x20]! | probe: conditional negate |
| ConcentrationCursor iterator get_Current | 0x2169ee0 | 8 | ldr x0, [x0, #0x18] | random game |
| nn.hid.GestureState/GesturePointArray4::ToString() | 0x1b21330 | 472 | sub sp, sp, #0x50 | random game |
| DynamicBoneCollider::OnValidate() | 0x187b3e0 | 40 | ldp s1, s2, [x0, #0x30] | random game |
| DebugMenu/DebugMapCollisionVisible::.cctor() | 0x1c6d440 | 128 | stp x29, x30, [sp, #-0x30]! | random game |
| nn.hid.ControllerSupportArg/Color4u8Array8::set_Item | 0x1b1f020 | 216 | stp x29, x30, [sp, #-0x20]! | random game |
| nn.hid.TouchScreenState10 enumerator get_Current | 0x1846ec0 | 28 | ldr x13, [x0, #0x18] | random game |
| MessageDataFilter::decode_button_Y | 0x1963fa0 | 208 | sub sp, sp, #0x60 | random game |
| Battle.BattleEvent::trigger_2059_11() | 0x1d339f0 | 196 | stp x29, x30, [sp, #-0x20]! | random game |
| Battle.BattleAnimExcel lambda (crystal revive effect) | 0x1b847b0 | 336 | stp x29, x30, [sp, #-0x50]! | random game |
| Unity.Burst.Intrinsics.Arm/Neon::vpadd_s16 | 0x164e760 | 64 | stp x29, x30, [sp, #-0x20]! | random engine |
| UIElements UIRStylePainter::get_closingInfo() | 0x1f392a0 | 36 | ldp x9, x10, [x0, #0x90] | random engine |
| Rendering.DebugUI/Container::get_panel() | 0x181dd90 | 8 | ldr x0, [x0, #0x10] | random engine |
| TMPro.TMP_Text::get_mesh() | 0x1a72640 | 8 | ldr x0, [x0, #0x398] | random engine |
| CriWare.CriWareErrorHandler/Callback::Invoke(string) | 0x19d8050 | 20 | ldr x8, [x0, #0x40] | random engine |

The picks use seed 20261124, so the table is reproducible. A method that ends in a `bl` to a no-return helper (a managed throw) counts as terminated; that rule came from the Burst `vpadd_s16` case.

## AC4: origin of every .text function

157,221 functions: 152,217 have an eh_frame FDE, 5,004 do not (found by splitting FDE-less code gaps at call targets and post-return starts). Bytes covered: 36,184,836 of 36,847,096 (the rest is alignment padding).

| Class | Functions | Bytes | Tier A | B | C | D |
|---|---|---|---|---|---|---|
| unity_native (Unity engine C++) | 57,395 | 18,613,548 | 0 | 4,652 | 51,765 | 978 |
| unity_bcl_csharp (BCL, Unity and package C#) | 47,840 | 9,539,916 | 47,835 | 0 | 0 | 5 |
| il2cpp_glue (invokers, adjustor thunks, interop) | 20,461 | 1,504,428 | 18,595 | 0 | 1,748 | 118 |
| game_csharp | 16,376 | 4,382,084 | 16,376 | 0 | 0 | 0 |
| cri_native (CRIWARE C) | 5,756 | 1,008,736 | 0 | 1,322 | 3,983 | 451 |
| thirdparty_c (FDE-less C library region) | 3,973 | 242,820 | 1 | 3,961 | 0 | 11 |
| libil2cpp (runtime) | 2,199 | 568,980 | 0 | 386 | 1,409 | 404 |
| cri_csharp | 2,187 | 262,464 | 2,187 | 0 | 0 | 0 |
| nn_sdk (import thunks, dynsyms) | 700 | 13,596 | 700 | 0 | 0 | 0 |
| libc_libcxx (imports, dynsyms) | 334 | 48,264 | 284 | 0 | 3 | 47 |
| Total | 157,221 | 36,184,836 | | | | |

Tiers:

- A is exact: a registration pointer table entry, a dynsym, or an import thunk.
- B is strong: a path or library string referenced by the function, or membership of the FDE-less region.
- C sits between two same-class anchors that agree.
- D is a boundary resolved by call-graph vote or nearest anchor. Weakest; 2,014 functions (1.3 percent).

Managed code split: game_csharp is Assembly-CSharp 15,057, MeshBakerCore 1,317, generated stubs 2. cri_csharp is CriMw.CriWare.Runtime 2,182 and Vp9 5. unity_bcl_csharp is led by Unity.Mathematics 9,336, mscorlib 9,302, UIElementsModule 4,943, CoreModule 4,916, Burst 3,627.

il2cpp_glue split: adjustor thunks 9,331, invokers 7,798, interop 2,008, mixed 883, unresolved virtual 428, reverse P/Invoke 13.

Two classes differ from the card's list: `thirdparty_c` (about 4,000 FDE-less functions in one region; the C library inside is not identified) and `il2cpp_glue` (kept apart from libil2cpp because the registration tables name them exactly). The card's "libc/libc++" class is small (334) because musl and libc++ are imported from `nnSdk.nss`, not linked into `main`; 256 of the 334 are import thunks. No function stayed unknown.

Validation of tier C: hide each tier A or B band anchor (95,314 anchors, point-only evidence excluded) and predict it from its neighbors. When both neighbors agree (95,041 cases) the prediction was right 95,019 times, accuracy 99.977 percent. The 22 misses are mostly cri_native next to unity_native (13) and game_csharp next to BCL (4). Data: `loo_check.json`.

## Hot callees

The functions with the most distinct callers are the runtime entry points a recompiler must provide first. Full top 300 in `hot_callees.tsv`.

| RVA | Distinct callers | Class | Note |
|---|---|---|---|
| 0xc4150 | 24,470 | il2cpp_glue | unnamed, 4 bytes |
| 0xc4330 | 13,977 | il2cpp_glue | unnamed, 4 bytes |
| 0xc44d0 | 10,303 | il2cpp_glue | unnamed, 4 bytes |
| 0xc4090 | 7,745 | il2cpp_glue | unnamed, 4 bytes |
| 0xc4300 | 7,163 | il2cpp_glue | unnamed, 16 bytes |
| 0x3fa50 | 6,623 | libil2cpp | unnamed, 12 bytes |
| 0x5b7c40 | 5,393 | unity_native | refers to the string `BurstException` |
| 0x153a170 | 4,210 | libc_libcxx | import thunk for `memcpy` |
| 0x1deb8e0 | 3,230 | unity_bcl_csharp | `System.Object::.ctor` |

The roles of the unnamed entries are not decoded yet; they are the first thing to read when the recompiler's runtime shim is designed.

## Ghidra and other tool outputs

- `ghidra_apply_il2cpp.py` applies names, plate comments, origin-class names, import labels and C# structs (category `/il2cpp`). It is syntax-checked only: Ghidra was not run, as the card forbids it.
- Import `main_v101.elf` (base 0x7100000000 baked in, 157,221 function symbols) with the ELF loader, decline auto-analysis, then run the script.
- `ghidra_names.tsv` has 85,978 named functions, `ghidra_classes.tsv` 71,243 more that get `<class>_<rva>`, `imports.tsv` 2,259 import slots, `ghidra_structs.tsv` 7,407 types and 33,627 fields.
- Struct layout check: Vector3 is 12 bytes (x, y, z at 0, 4, 8); reference types carry the 16-byte object header. Metadata offsets for value types include a 16-byte box header and are corrected on export.

## Cross-check against Il2CppDumper and Cpp2IL

Il2CppDumper 6.7.46 ran on the same NSO and metadata (about one minute, BelowNormal priority, outputs in `crosscheck\dumper\`, none in Git). Our parsers stay the source of truth. `crosscheck\report.json` holds the numbers.

| Check | Result |
|---|---|
| CodeRegistration and MetadataRegistration | Same addresses: 0x33c6828 and 0x33c7820 |
| Types | 8,045 in `dump.cs`, equal to ours |
| Method addresses | All 63,227 of ours appear in the dumper map at the same address. 0 ours-only. |
| Method names at those addresses | 63,227 of 63,227 equal (type and method name, nested types written with `.`) |
| Dumper-only methods | 3,171, and every one is a generic method instance address from our `generic_methods.tsv`. 0 unexplained. |
| Instance field offsets | 7,971 types identical; 74 are open generic value types where both tools give all zeros (our value type offset shift is the only difference). 24,826 instance fields on each side. 0 real differences. |
| String literals | Dumper lists 11,183, ours 11,189. The 6 extra are in the metadata but unreferenced by code. 0 dumper-only. |
| Match rate | 100 percent on addresses, names and field offsets; no disagreements |

The dumper also confirmed full generic sharing: 2,225 of its generic method names contain `__Il2CppFullySharedGenericType`. Its metadata usage list has 14,590 method slots, 7,378 with a code address (all inside our map) and 7,212 with none.

Cpp2IL 2022.1.0-pre-release.21 failed. It found the MetadataRegistration at 0x33C7820 (same as ours) but not the CodeRegistration ("Hit backtrack limit of 59 modules"), then stopped with a fatal error. Not retried; it adds nothing the dumper did not already confirm.

## Failed or abandoned hypotheses

1. LZ4 decode at the header offset failed: the file offset in the NSO header is 0x101, not 0x100. Decoding at the header value works and the hashes match.
2. RTTI type names as an anchor source: the build uses `-fno-rtti`, only 6 typeinfo objects exist. Dropped.
3. Nearest-neighbor from `operator new/delete` dynsyms bled libc_libcxx over about 8,000 functions. Fixed: point-only evidence (import thunks, small dynsym groups) no longer propagates; a band needs at least 10 same-class exports within 32 KB.
4. A curl string rule labeled Unity web request wrappers as third-party C. Fixed: `thirdparty_c` is now decided only by the FDE-less region.
5. One run of Unity functions had no strong anchor. Fixed with a weak Unity vocabulary rule (needs 3 hits).
6. Spot check first reported 0 plausible (a regex needed leading whitespace), then two false failures (the `Mathf.Abs` probe and a Burst function ending in a throw call). Checker fixed; the code under test did not change.

## Risks for the recompiler

- Exceptions: only 1,929 functions have an LSDA (call-site tables): BCL C# 1,065, game C# 393, libil2cpp 371, CRIWARE C# 81, glue 18, libc++ 1. Unity native code has none. All of it goes through `__gxx_personality_v0` and the unwinder, which are imported from `nnSdk.nss`, so the recompiler needs its own unwinder or a mapping onto host C++ exceptions. A managed throw ending in a `bl` to a no-return helper is common (the checker needed a rule for it).
- GC: the binary embeds a Boehm collector (strings `GC_init_gcj_malloc: bad index`, `GC_DUMP_REGULARLY`) plus incremental collection entry points. It scans stacks, registers and static data, so recompiled code must keep object pointers visible to it; a host-native register allocator is a hazard. Not yet verified at instruction level.
- Generic sharing: only 3,172 generic method pointers exist for 14,926 specs, and 2,225 of the named generic entries carry `__Il2CppFullySharedGenericType` (full generic sharing, value types included). Shared bodies take a hidden generic-context argument, and a call site reaches the body through a metadata usage slot filled at run time (14,590 method usage slots: 7,378 have a code address, 7,212 are resolved by the runtime). The recompiler must model that lookup. The exact runtime path of the slot fill is not decoded yet; this stays the largest open question from this task.
- Invokers and adjustor thunks: 7,798 invokers and 9,331 adjustor thunks are plain data-driven stubs (class A, exact). They can be generated or replaced wholesale instead of recompiled one by one.
- Value type layout: the 16-byte box header sits inside metadata offsets for value types; struct exports in `ghidra_structs.tsv` already subtract it.
- Tier C and D labels (60,922 functions) are inferred. Anything that gates a build decision on the class of a specific function should check the `conf` column in `functions.tsv`.

## Evidence files (all under `A:\AI\Recomps\smrpg-work\analysis\il2cpp\`)

`images.tsv`, `types.tsv`, `methods.tsv`, `fields.tsv`, `strings.tsv`, `generic_insts.tsv`, `generic_classes.tsv`, `generic_methods.tsv`, `registrations.json`, `functions.tsv` (157,221 rows: rva, end, size, FDE, LSDA, class, subclass, tier, evidence, name), `origin_summary.json`, `loo_check.json`, `spot_check.tsv`, `hot_callees.tsv`, `imports.tsv`, `ghidra_names.tsv`, `ghidra_classes.tsv`, `ghidra_structs.tsv`, `main_v101.elf`, `features.pkl` (cache).
