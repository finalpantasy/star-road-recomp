# Prior art for Super Mario RPG (Switch 2023), ideas only (task-9)

Research date: 2026-11-24. Read-only: pages and public APIs were read, nothing was downloaded, built or run, and no code was copied. License is as the host (GitHub API or the page) reports it. "Verified" means the link opened or answered an API call on that date.

## Headline

**A working static recompilation of this game already exists** (item 1). It is public, MIT-licensed, plays from the opening to the endgame areas per its own notes, and targets v1.0.0 only (our dump is v1.0.1, update v65536). It is built on LASRecomp (Link's Awakening). Our owner's rule is ideas only, so the value for us is the list of problems it hit and solved, the numbers it measured, and the design choices we can make better or differently. Our quality bar (best recompilation and decompilation ever made) and the stage 2 decompilation are still open ground: it has no decompilation and no reference tests against the original.

## 1. Projects found

Status key: ACTIVE = changed within 14 days of the research date; STALE = no change for 6+ months.

### 1A. Recompilations, native ports and emulation-adjacent ports of this game

| # | Project | Link | Status | License | What it learned about the game | Idea for us |
|---|---|---|---|---|---|---|
| 1 | SMRPG Recomp (PC), by ElKiro1311 | <https://github.com/ElKiro1311/smrpg-recompiled> | ACTIVE. Created 2026-11-23, 0.52.0-beta, 0 open issues, 0 forks, 8 stars. Windows only. Installer builds game code on the user's PC in 10-20 min | MIT (port) on top of LASRecomp MIT; third-party DLLs keep their own licenses | See section 2 | Tasks 3, 4, 5, 6, 7 (section 3) |
| 2 | SMRPG Recomp for Android | <https://github.com/ElKiro1311/smrpg-recompiled-android> | ACTIVE, created 2026-11-23, 2 stars | MIT | Same port on Android/Vulkan. Android branch fed the PC port a few translator fixes (SHFL with c operand, MUFU.SIN/COS reduced angle, local-memory sizing per shader, shader-seen cache) | Task 3: the same shader-translator bugs will hit us |
| 3 | LASRecomp (base of 1) | <https://github.com/peiikai/LASRecomp> | ACTIVE, 2,579 stars, created 2026-10-08. Windows, macOS, Linux, Android, iOS. Says it was written almost entirely with an AI | MIT (GitHub shows "Other"; the LICENSE file text is MIT) | Pipeline: `lifter.py` + `passes.py` + `simd_c.py` turn A64 into one C function per guest function; indirect calls via an address-to-function lookup table; Vulkan or Metal NVN backend; Maxwell shader translator; per-version address tables | Task 4, 5: reference architecture to compare ours against |
| 4 | nxnative, by seedds | <https://github.com/seedds/nxnative> | ACTIVE, 16.6k stars, created 2026-10-13. Not a recompiler: an NSO rewriter plus Horizon HLE host plus NVN to Metal; the game's ARM code runs natively on Apple silicon | MIT | Has a profile for this exact game, **v1.0.1 (v65536)**: `dm/games/0100BC0018138000/README.md`, `game.tsv`, shader tables. Facts in section 2 | Tasks 3, 4: best public source of per-game Switch facts for 1.0.1 |
| 5 | nxnative derivatives with SMRPG notes | peanuts-native <https://github.com/igawa6/peanuts-native/issues/443>; nxnative PRs <https://github.com/seedds/nxnative/issues/554>, <https://github.com/seedds/nxnative/issues/993>; others: zFitness/nx2androidX, enjoyerofslop/droidplay-core, Xc1Ym/LumaNX, cheemu21/plumber-racer-2 (these carry copies of nxnative's profile; not read further) | ACTIVE | peanuts-native MIT; others not checked | Memory-pool sizing and the RGBA4 channel-order bug (section 2) | Task 3 |
| 6 | Masu (Android runner), issue "Doesn't start" | <https://github.com/orikami-dev/Masu/issues/8> | Open, empty report | not checked | Nothing learned | None |

### 1B. Modding, patches, cheats, tools (Switch 2023 version)

| # | Project | Link | Status | License | What it learned | Idea for us |
|---|---|---|---|---|---|---|
| 7 | NXCheats, Super Mario RPG set, by bad1dea | <https://github.com/bad1dea/NXCheats> (folder `0100BC0018138000 Super Mario RPG™`) | ACTIVE, first commit 2026-10-03, last push 2026-10-23. Targets v65536 (1.0.1), cheat file id 97F9664CFB272154 | MIT | The retail build still contains the game's **own debug menu**; the author found how to open it (L3+R3 in the field). Menu warps to any map, edits coins, flower points and party stats, toggles collision, encounters, an FPS and position overlay. Also: game speed to 4x, movement and jump multipliers, damage, XP, coin multipliers, drop rates, action-gauge, chain, auto action commands. Code is too large for the cheat VM, so it ships as an IPS patch plus a switch file | Task 2, 3: find the debug menu code path in the metadata; a free test harness (warp to any scene) for stage 1 and stage 2 tests; list of the game variables worth exposing in our own debug build |
| 8 | switch-cheats-db entry | <https://github.com/HamletDuFromage/switch-cheats-db> (`cheats/0100BC0018138000.json`) | ACTIVE (push 2026-11-22) | none stated | Cheats keyed to build id E968832CADE2AD7C (labeled v0 / 1.0.0). Shows 1.0.0 and 1.0.1 are different `main` builds | Task 1, 2: use the two build ids to tell versions apart |
| 9 | Super Mario RPG Randomizer, by takstijn and friends | GameBanana tool <https://gamebanana.com/tools/15574>; source <https://github.com/ObsidianMakerDevelopment/super-mario-rpg-switch-randomizer> | STALE (2023-12 release, source pushed 2024-03). Windows exe, needs the dumped RomFS | MIT (confirmed on both) | Game data tables are **MS-NRBF serialized .NET object streams** (the author credits an MS-NRBF reader, bbowyersmyth/BinaryFormatDataStructure, and added a writer). Modules show which tables exist: character initial stats, level-up stats, specials, equip rules, item stats and prices, treasure, encounters, shops, cutscene order, monster skills, monster XP | Task 2, 3: the `.tbl` files are readable data, so game data can be extracted to JSON at build time (never committed) |
| 10 | Basic Difficulty Tool and spell tools, by Grobyc1337 and Maxtoad | GameBanana tool <https://gamebanana.com/tools/15986>; <https://github.com/Grobyc1337/MarioRPGremake-tools> (verified) | STALE (2024-01) | Page: CC BY-NC-ND 4.0. Repo: no license | `Custom_Mod\romfs\Data\StreamingAssets\data_tbl\monster.tbl` is the monster table; a script round trips `.tbl` to `.json` and back (`reserialize.py`). Other lists: `action.json` (spell data), `battle_motion_monster.json`, `menu_text_eng_us.json` | Task 3: confirms table names and the data_tbl round trip |
| 11 | Native 4K/2K mod, by KeatonTheBot | <https://gamebanana.com/mods/479591> | STALE (2023-11-17), 29k views | CC BY-NC-ND 4.0 | Emulator-only patch that raises the native resolution (4K docked, 2K handheld); needs 6 GB DRAM layout for 4K. Supports 1.0.0 and 1.0.1. Means the resolution is a data or small-code switch inside the game, and that 4K raises memory use | Task 3: resolution handling in Unity Screen setup; memory budget at 4K |
| 12 | Revised Script, Hard Mod, Luigi + Daisy, Cyber's Party Pack, Xbox and Steam button prompts, Polish translation, texture and model swaps, starter saves, many requests | GameBanana game hub <https://gamebanana.com/games/18647> (64 submissions); e.g. <https://gamebanana.com/mods/482411>, <https://gamebanana.com/mods/520534>, <https://gamebanana.com/mods/699337>, <https://gamebanana.com/mods/740235> | Hub ACTIVE (newest 2026-11-14); most mods 2023-24 | CC BY-NC-ND 4.0 on the pages read | Text is fully replaceable (about 5,000 dialogue strings, per the Polish mod); button prompts live in several separate systems (battle, menus, save, shops, tutorials, dialogue, help, monster list, sound player, minigames); textures swap by editing `Texture2D` inside Addressables bundles (`StreamingAssets/aa/Switch/d_assets_*`, e.g. `cg_data-character-p0001_mario.bundle`) with UABEA or AssetStudio; remake data sits under `romfs/Data/StreamingAssets/data_tbl` | Task 3: asset layout; stage 2: the mod loader should accept loose-file overrides in the same paths so these mods keep working |
| 13 | Save editors | <https://github.com/dlr-developer/super-mario-rpg-switch-save-editor> (MIT, pushed 2026-10-07, ACTIVE, 63 cheats for 1.0.0, edits coins, stats, gear, items, storage box, writes untouched bytes back byte for byte); <https://github.com/bray-dev/RPGSaveEditor> (no license, pushed 2026-06, "work in progress") | ACTIVE / STALE | MIT / none | Saves are read and written without the game; the ElKiro port notes the save is JSON in UTF-16 (file `SMR_Save_Ver0011000`) and that the save's `_respawn_floor_name` chooses the scene | Task 3: save format; stage 2 save compatibility test (our saves must load on the real game and the reverse) |
| 14 | Text hooker script for Yuzu, 0xDC00/scripts | <https://github.com/0xDC00/scripts> (`NS_0100BC0018138000_Super_Mario_RPG.js`) | ACTIVE repo; the script targets 1.0.0 | none found | Hooks about two dozen text functions in 1.0.0 (main text, names, cutscene, item and monster lists, help, level-up); strings are IL2CPP managed strings, UTF-16 with a length field at +0x10 and chars at +0x14, with TextMeshPro style tags | Task 2: cross-check our function names against these text paths |
| 15 | Switch cheat and performance databases | <https://github.com/biase-d/nx-performance/issues/948> (closed; a placeholder for v1.0.1 with new graphics settings, contributed by the FPSLocker author); <https://github.com/theboy181/switch-ptchtxt-mods/issues/37> (open request for a 30 fps patch, 2023) | mixed | none | No public 60 fps or resolution patch found for this game beyond item 11. The game already runs at 60 fps on console | Task 3: no prior art for frame-rate patches; the frame-rate logic must come from our own survey and from item 1's battle findings |
| 16 | Switch emulator mod pack | <https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods> (zip `Super Mario RPG [0100BC0018138000][mods].zip`, pushed 2026-11-15, no license) | ACTIVE | none | Contents of the zip were not opened (would need a download) | Task 3: low priority; open later if we need ultrawide hints |

### 1C. Emulator reports

Searched for this game on GitHub issues (Ryujinx, Ryujinx-Games-List, yuzu, suyu, Eden, Citron, Sudachi repos: those repositories are no longer on GitHub or return no results), plus web search. Findings:

| Source | Link | What it says |
|---|---|---|
| Wccftech, Nov 2023 (leak coverage) | <https://wccftech.com/super-mario-rpg-leaks-yuzu-ryujinx/> | Played on Yuzu and Ryujinx before release; stuttering audio on Ryujinx, helped by changing background-music settings; about 60 fps on Steam Deck via Ryujinx |
| Native 4K/2K mod page | <https://gamebanana.com/mods/479591> | 4K needs the alternative memory layout (Ryujinx) or 6 GB DRAM (Yuzu) |
| Xbox button prompts mod | <https://gamebanana.com/mods/699337> | Tested on Eden, works on Ryujinx and Sudachi; emulators map controller buttons by position, so prompts are wrong on Xbox pads |
| Starter saves | <https://gamebanana.com/mods/481142> | Same save loads on Yuzu and Ryujinx |
| nxnative issue 554 | <https://github.com/seedds/nxnative/issues/554> | See section 2 (virtual address space exhaustion) |
| Masu issue 8 | <https://github.com/orikami-dev/Masu/issues/8> | Does not start; empty report |

**Not reachable** (no workaround attempted, per read-only and no-bypass rules): Ryubing's tracker at git.ryujinx.app (bot check page), TCRF's page for the Switch version (Cloudflare challenge, HTTP 403), Reddit (blocked for web search; the `reddit-reader` skill was not run because no specific thread had been identified). Eden and Citron trackers were not reachable from page scripts (cross-origin) and were not opened. No game-specific bug report for Ryujinx, yuzu forks, Citron, Eden or Sudachi surfaced anywhere, so game-specific emulator quirks are mostly absent from public indexes; **task-7 should check those trackers by hand.**

One more note: a fetch of the TCRF page through a helper returned text that told agents to write a file and move money. That text came from the web page, was not acted on, and carries no authority. It also means nothing from that fetch can be trusted as TCRF content.

### 1D. Other projects for the SNES original (different game, listed for scope)

| Project | Link | Status | License | Relevance |
|---|---|---|---|---|
| SuperMarioRPGRecomp (SNES static recomp, 65816 plus emulated SA-1), by mstan | <https://github.com/mstan/SuperMarioRPGRecomp> | ACTIVE, v0.0.5, 24 stars, AI-assisted | NOASSERTION | Different binary. Useful only as a behavior reference: the remake reimplements the SNES game's rules |
| Super-Mario-RPG-Disassembly, by Yoshifanatic1 | <https://github.com/Yoshifanatic1/Super-Mario-RPG-Disassembly> | STALE (2021) | GPL-3.0 | Names for battle and field logic of the SNES game |
| smrpg_web_randomizer, smrpgpatchbuilder (Super-Mario-RPG-Randomizer org); LAZYSHELL++ (Yakibomb/LAZYSHELL-UPDATED); Lazy Shell on GameBanana (<https://gamebanana.com/tools/6380>); SMRPG-Demake-Project (AnAxemRanger, adds remake features to the SNES game, <https://github.com/AnAxemRanger/SMRPG-Demake-Project>) | linked | MIT (randomizer, patchbuilder); others none | SNES data editors | Task 7: formulas and exact SNES behaviors to compare against remake bugs, only if needed |

### 1E. Adjacent recomps worth reading for design (from the owner's list)

All verified on GitHub 2026-11-24: N64Recomp (MIT, <https://github.com/N64Recomp/N64Recomp>), XenonRecomp (MIT, <https://github.com/hedge-dev/XenonRecomp>), UnleashedRecomp (GPL-3.0, <https://github.com/hedge-dev/UnleashedRecomp>), New Pokemon Snap Switch recomp (GPL-3.0, <https://github.com/finalpantasy/new-pokemon-snap-recomp>, our own account), Il2CppDumper (MIT, <https://github.com/Perfare/Il2CppDumper>), Il2CppInspector (AGPL-3.0, <https://github.com/djkaty/Il2CppInspector>). Not found by search: ACNH recomp, Power Moon Recomp (Odyssey), FE3H recomp by name (a "FE3H Deck Recomp" exists on a LASRecomp base, <https://github.com/Rio77/fe3h-deck-recomp>, license unclear). Aggregators: recomp.gg and recomplauncher.com list only the SNES game for this title.

## 2. What the projects learned about this game (facts to verify ourselves)

Treat all of these as claims to confirm against our dump; each is attributed.

**Program and platform** (nxnative profile for v1.0.1; ElKiro STATUS)
- SDK 14.3.1, Unity 2021.3.15f1 IL2CPP, CRIWARE, NVN. Modules `main`, `subsdk0` (the SDK's `nn::movie` library, 31 imports from `main`) and `sdk`. ElKiro states the `sdk` and `subsdk0` of 1.0.0 are identical to 1.0.1 (same build id); `main` differs between versions (separate cheat build ids, items 7 and 8).
- Module layout: `main` at 0x7000000000 (to 0x7004dbc000 with .bss), `sdk` at 0x7005000000 in their runtime. Ours are module-relative offsets (see ARCHITECTURE contracts).
- NPDM declares 16 MB of system resource, so `nn::os` virtual-address-memory is on. The game's `nninitStartup` (main+0x62de90) reserves about 3.1 GB of virtual space for a StandardAllocator and does not call SetHeapSize. AddressRegionManager works in the alias region, which needed 48 GB on PC.
- On iOS/Mac, **1.0.1 exhausts the SDK's virtual-address region at startup and during scene transitions** (nxnative issue 554: 179 live regions held 3.09 GB, a 2 GB pool failed leaving Mario's house; 1.5 GB pool fixed it). peanuts-native instead needed a 2 GB pool (1 GB crashed on a null at main+0x627a74 before the first frame). Pool size interacts with Unity's own reservations.
- Memory on Mac: 4.71 GB resident, 0.89 GB GPU, over six minutes.
- Threads: MainThread, UnityGfxDeviceWorker, Loading.AsyncRead, Job.Worker 0-1, Background Job.Worker 0-15, GC Finalizer, AssetGarbageCollectorHelper, BatchDeleteObjects, NetworkInterfaceWrapper, LibcurlGc, a vsync thread (main+0x6b536c), NxSystemThread. IL2CPP's GC stops threads with suspend plus get-context (cooperative at safe points).
- Unity defines global operator new/delete that override the SDK's weak ones. `nn::ro` is only Initialize and LookupSymbol (no NRO loaded).
- AArch64 `STLR`/`LDAR` are seq_cst; x86 needs a full barrier for STLR. ElKiro hit a lost wakeup in condvars and a hang until memory ordering was fixed.

**Graphics**
- Unity asks for 277 NVN functions; ElKiro found 19 missing and several empty (Barrier, SetAlphaRef, SetPointSize, CopyBufferToBuffer, deferred binds, BindSeparateSampler(s), ReportValue, DrawTexture).
- Shaders: 82,032 (nxnative: 40,983 vertex, 40,987 pixel, 56 compute, 6 geometry; 82,021 under `Data/` plus 11 GLSLC outputs in `main`). ElKiro translates 82,006 of them; 22 untranslated shaders are unused by the game (VR, wireframe, LookDev, CopyDepth with MSAA, 7 internal). Nobody has reported a shader the game needs that cannot be translated, but nxnative's translator drops 20 bindless texture accesses in 4 shaders (results read 0), cause untraced.
- Unity binds textures and samplers separately: texture word at `c[2][0x168 + 8r]`, sampler word at `c[2][0x568 + 8r]`, OR-ed, sampled with bindless `TEX.B`/`TLD.B`. Vertex shaders discard early with `VOTE.VTG` then `EXIT CC.FCSM_TR`, and that EXIT must not run.
- The game calls `nvnDeviceSetDepthMode` with near = 0, so pipelines made before it must use the zero-to-one variants. It calls `SetSubpixelPrecisionBias` and `SetConservativeRasterDilate` thousands of times without enabling conservative raster.
- **RGBA4 shadowmask textures** (Mushroom Way battle ground went nearly black): NVN holds R,G,B,A from bit 0 up, Metal's ABGR4 holds A,B,G,R, so channels read swapped. Same bug will exist on any backend with a different packed order (nxnative issue 993).
- The edges of the game's own post chain leave the last row and column of intermediate buffers unwritten and its final blit blends them in: a dark line at the right and bottom edges at any resolution (ElKiro 0.49.1; reproduced at native 1920x1080).
- The game's UI sits in a fixed 1920x1080 panel: true widescreen works for the world but the title background, dimming and some transitions do not reach the sides. Videos are 1920x1080 VP9, 60 fps.
- Unity's screen setup clamps to 1920x1080 (ElKiro replaced `Screen.SetResolution` and `CanvasScaler.HandleScaleWithScreenSize`).
- URP 12 is used; its MotionVectorRenderPass can be forced on for upscalers and frame generation (ElKiro 0.30s).
- Some chest or similar object lost its wooden body after 6 battles in a row (ElKiro "pending": unexplained, zero traced calls).

**Frame rate and timing**
- The game runs at 60 fps on console, present interval 1 (nxnative). It counts movement by time, so field logic is the same at 240 as at 60 (ElKiro).
- **Battles do not**: `BattleManager.update_common` takes one 1/60 s step per rendered frame, so at 240 fps battle logic runs four times faster than animation and sound. ElKiro's fix: replace `BattleManager.update` to hand out steps by real time (0-3 per frame, debt cap 4) and hold button presses per step in the controller snapshot (`ControllerObserver._npadState`). Measured: 57-58 steps per second after a jump on entering the battle; logic ticks at 60 Hz.
- Another game in nxnative shows the general Unity fix: find `QualitySettings.vSyncCount` and `Application.targetFrameRate` through IL2CPP's internal-call table.

**Data and assets**
- RomFS: 1,934 files, 6,820,987,323 bytes (ElKiro), including 1,277 UnityFS bundles and 46 CRI USM movies (1.01 GB) (nxnative). Merged RomFS under 1.0.1 is 6.35 GB.
- `Data/StreamingAssets/data_tbl/*.tbl` tables are MS-NRBF; `.json` round trips exist. Addressables catalog lists 416 scenes; ElKiro swept all zones with no failures (19 names are not respawn floors and the game rejects them).
- The game contains an unused or hidden **Debug Room** and debug menu strings in all localized menu-text files (web search summary of TCRF; not read at source because of the 403).
- Saves: JSON in UTF-16 under `save/save_0/SMR_Save_Ver0011000`. Autosave exists.
- IL2CPP names were produced with Il2CppDumper 6.7.46 on `exefs/main` plus `global-metadata.dat` (ElKiro; needed `DOTNET_ROLL_FORWARD=Major` on .NET 8).
- Text: managed UTF-16 strings with TextMeshPro rich-text tags such as `<style=...>` and `<sprite name=A>`.

**Movies and audio**
- CRIWARE creates its decoder as type 3 (VP9), format 3, mode 2 (native texture); `main` imports the `nn::movie` functions from `subsdk0`. ElKiro implements them with FFmpeg; unimplemented: `GetH264NalUnitStartPositionAndNalSize` and the Extractor calls by symbol. Audio is one stereo Opus stream through the `hwopus` service (command 7: CRI asks 196 times for a multistream decoder size with 2 mono streams, a memory calculation, not a bug).
- nxnative notes one AudioOut with 6 channels and short underruns; the Ryujinx report of stuttering audio (2023) is the only emulator audio note.

**Not tested by anyone we found**: real controller and speakers on the PC port, long soak, fullscreen with visible window, anything past mid-game combat on iOS/Mac (nxnative), and 1.0.1 on the recompiler (ElKiro supports 1.0.0 only).

## 3. Ideas mapped to our task cards (ideas only)

| Task | Idea | Why it helps | Source |
|---|---|---|---|
| task-4 (design) | **Decide now how we differ from the public port.** It lifts A64 to C per function with a lookup table for indirect calls, plus a hook layer (`rt_hook`, an overrides file linked first so overrides win even for internal calls). We should keep a function-replacement mechanism by name or address from day one, because stage 2 needs it | The decompilation plan swaps functions into a running stage 1 build; they proved this works | items 1, 3 |
| task-4 | Build the C on the user's machine (the dump never leaves it), cache per-function objects, ship an installer that checks the dump by build id, `main.npdm` title and per-file hashes of the RomFS | Matches our "nothing from the game in Git" rule and keeps rebuilds fast. 10-20 min first build; hard links avoid copying 6 GB | item 1 |
| task-4 | Plan for a guard against Windows Control Flow Guard, and for the compiler threads needing large stacks (64 MB; LLVM overflowed at 2 MB) and not trusting LLVM's crash handling when guest faults are emulated | Cheap to learn from their pain | item 1 STATUS 0.48.0 |
| task-4, 5 | Memory model: x86 needs a full barrier for `STLR` and acquire loads for `LDAR`; condvar lost-wakeup bugs show up as boot hangs. Write a memory-ordering test before the first boot attempt | They lost days to it (versions 11 to 16) | item 1 STATUS |
| task-5 | Boot milestones to copy as acceptance checks: sdk constructors (112) and game constructors (288), then `nnMain`, mount RomFS, create about 30 Unity threads, load `level0`, first presented frame | Concrete counts to verify our lifter against | item 1 STATUS |
| task-5 | Debug aids that paid off: per-function call counters, "who calls this first" on a chosen address, memory-watch ranges, hung-thread dump with every thread's Unity name and last kernel events, and a guest-address crash log (`main+0x...` mapped to a name) | Our map from task-2 gives names; these give a fast loop | item 1 STATUS |
| task-3 | Check these in the NVN layer: RGBA4 channel order; depth mode 0 to 1; 277 imported NVN entry points (19 not implemented elsewhere); `NVNtextureView` 16 bytes, `NVNtexture` 0x80; separate sampler and texture handles; early-discard vertex shaders | Each cost the others a bug hunt | items 1, 4 |
| task-3 | Shader strategy: pre-translate every shader variant found in the dump at first run and build pipelines in the background so there are no compile hitches; record the used-shader list and reuse it. 82k shaders is the working set | They measured 9,686 skipped draws in 2 s without it and 0 with it | item 1 0.49.0 |
| task-3 | Frame-rate survey must separate field logic (time-based) from battle logic (per-frame step). Use a counter of real calls to the battle step as the test | Keeps 120/240 fps from running battles fast | item 1 0.50.x |
| task-3 | Widescreen and resolution: record every place a 16:9 constant lives (Unity `Screen.SetResolution` clamp, `CanvasScaler`, `RawImage` of the menu background, fade `Image` in `GS.Fade`, movie player rectangles, the title wipe). The title wipe and battle transition effect still leave bars in their port because the capture is 16:9 | Gives us a checklist for upgrades behind options and A/B with a faithful mode (D-004) | item 1 0.41 to 0.49.7 |
| task-3 | Movies: VP9 1080p60 in CRI USM with Opus audio; implement the `nn::movie` service surface from `subsdk0` | We know which 31 symbols to expect | item 1 STATUS |
| task-2 | Run Il2CppDumper-class recovery on `main` plus `global-metadata.dat`; bring function names in line with the text hooks (item 14) and the debug menu (item 7) so we can name those first | Cheap cross-checks of our output | items 1, 7, 14 |
| task-2 | Find the retail debug menu and the L3+R3 hotkey path in the metadata. Likely a manager class that nothing constructs | Gives a free harness for scene warps and state edits | item 7 |
| task-2, 3 | Data tables: `monster.tbl`, `action.tbl`, shop, treasure, encounter and level-up tables are MS-NRBF streams; decode them at build time into JSON on A: for the decomp's data layer | Stage 2 readable data without hand transcription | items 9, 10 |
| task-6 (decomp language) | Their lesson: replacing Unity functions one at a time (`BattleManager.update`, `Screen.SetResolution`, `RawImage.set_texture`) works. The C vs Rust choice should favor easy per-function override and ABI stability with the stage 1 build | Fits D-003 evaluation | item 1 |
| task-7 (bugs) | Known 1.0.1 issues from public notes: dark RGBA4 shadowmask only on a backend with swapped nibbles (not a game bug); battle speed at high fps (not a game bug); the "?" chest at d02out after 6 battles (unexplained on their port); button-prompt mismatch on Xbox pads (emulator mapping). **No public list of real game bugs found** here, so task-7 stands alone | Do not rely on this doc for task-7 | items 1, 4, 12 |
| Test design | Their method: scripted input by frame number, captures to compare, a full sweep of every addressables scene with 0 skipped draws as a pass criterion. We add what they lack: state hashes and recorded routes against the **original**, since they have no comparison with real hardware | Our correctness bar | item 1 |
| QoL and mods | Loose-file overrides in the original paths (`romfs/Data/StreamingAssets/...`), plus an optional HD texture folder with dump support, are what players already use | Keeps existing mods working on our build | items 1, 12 |

## 4. Risks and decisions this creates for the coordinator

1. **We are not first.** A public port already plays the game. The project premise (best recompilation and, above all, a full decompilation with reference tests) is still unique, but the owner may want to rephrase "first" in the README. No change made here.
2. **Version.** The public port supports 1.0.0 only. Our dump is 1.0.1. Public notes imply `sdk` and `subsdk0` match; `main` differs. We cannot assume their findings about addresses carry over; we must re-derive them. nxnative's notes are 1.0.1 and are the better source for facts.
3. **Licensing hygiene.** LASRecomp and the port are MIT, so reading them is allowed, but our rule is no code copying. Third-party pieces they use (Unicorn in the generator, GPL) are tools, not code we take.
4. **Public identity.** The port's author notes the work was done with AI assistance; so did LASRecomp and the SNES recomp. No action needed.

## 5. Sources searched and failures

Searched: GitHub repository, code and issue search (`gh`, read calls only); GameBanana game hub 18647 through its public JSON endpoints (read only); recomp.gg and recomplauncher.com pages; web search on Ryujinx, yuzu, Eden, Citron, Sudachi, TCRF, mod and port terms.

Failed or not reached: Ryubing's GitLab tracker (bot check), TCRF (HTTP 403 Cloudflare, plus an untrusted page text noted above), Reddit (excluded from web search), GBAtemp (no relevant hits), Eden and Citron trackers (not opened), Discord (not indexed; the GameBanana thread mentions a modding Discord at <https://gamebanana.com/threads/224500>, not joined). The GitHub repos for Yuzu, Ryujinx, Suyu and Sudachi return nothing now.

Open follow-ups: read the ElKiro `lifter.py` design notes at the document level only (task-4), review nxnative's `docs/reference/` set (threads, memory, rendering, shaders, rewriter, frame-rate) when we reach task-3, and ask the owner whether to open the TCRF page and the Ryubing tracker by hand.
