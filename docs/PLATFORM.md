# Platform layer (task-22)

`runtime/platform/` is the host side of the game on SDL3: one window, the Vulkan surface, keyboard and controller input written into the hid shared memory the game reads, and audio output. Our own code. SDL3 (zlib license) comes from `A:\msys64\ucrt64`; nothing from SDL is committed. `tools/rtprep/build_game.py` copies `SDL3.dll` and `libiconv-2.dll` beside `smrpg.exe`.

## Flags and environment

| Flag or variable | Effect |
|---|---|
| (none) | Window created hidden, never shown. Audio off: no SDL audio subsystem, no device, audout is a null sink. Input only from a script, if one is given. |
| `--window` | Show the window. Only the owner runs this. `F11` or `Alt+Enter` toggles fullscreen. |
| `--fullscreen` | With `--window`: start fullscreen. |
| `--audio` | audout PCM goes to the default playback device through an SDL audio stream. Only the owner runs this. |
| `--input-config PATH` | Keyboard map ini (also `SMRPG_INPUT_CONFIG`). Without it: `input.ini`, then `input.default.ini`, beside the exe, then the built-in map. |
| `SMRPG_INPUT=frame:BUTTON[:length],...` | Scripted pad input for tests. The keyboard and gamepads are then ignored. |
| `SMRPG_INPUT_FILE=path` | Same entries, one per line or comma separated. |
| `SDL_AUDIO_DRIVER=dummy`, `SDL_AUDIODRIVER=dummy` | Set by `tools/rtprep/run_game.py` and by the platform tests for every background run (owner rule: all background runs are muted). |

Scripted input: `frame` counts presented frames (NVN `QueuePresentTexture`), `length` defaults to 10 frames. Buttons: `A B X Y L R ZL ZR PLUS MINUS LSTICK RSTICK LEFT UP RIGHT DOWN`. Left stick: `STICK_UP STICK_DOWN STICK_LEFT STICK_RIGHT` or `STICK_<degrees>` (0 right, 90 up); right stick `RSTICK_...`. Overlapping entries OR their buttons and sum their sticks. Example: `SMRPG_INPUT="100:A:30,200:STICK_UP:40,260:PLUS:5"`.

## Default keyboard map

`config/input.default.ini` (the same as the built-in map, checked by the unit test): `L` is A, `K` B, `I` X, `J` Y (the right-hand diamond), `Space` also A, `Q`/`E` L/R, `1`/`3` ZL/ZR, `Return` plus, `Backspace` minus, arrows d-pad, `W A S D` left stick, keypad `8 5 4 6` right stick, shifts the stick clicks. Gamepads need no map: SDL's controller database does it; the east button is A, the south button B (Pro Controller layout, not Xbox letters). The first gamepad joins player 1 together with the keyboard, the second is player 2, and so on up to four.

## Pieces

| File | Role |
|---|---|
| `plat_core.c` | SDL init, window (hidden unless `--window`), event loop on the main thread, 5 ms (200 Hz) pad publish, scripted input, Vulkan surface, window state log line |
| `plat_nvn.cpp` | registers an `nvn::WindowProvider`; NVN makes its swapchain on the SDL surface. Without a provider NVN keeps its hidden Win32 window (the NVN unit tests rely on that) |
| `pad.c`, `pad.h`, `padstate.h` | pure mapping: SDL events to Pro Controller state, keyboard map ini, script parser; no window needed |
| `hidshm.c`, `hidshm.h` | writer for the 0x40000 byte hid shared memory block; also built into the HLE set through `hle/hle_hidshm.c` |
| `plat_audio.c` | SDL audio stream sink; nothing touches SDL audio unless `--audio` |
| `hle/hle_hid.c` | `hle_hid_publish` writes the state through `k_shmem_ptr` every tick (the guest may map the block later than the first write) |
| `hle/hle_audio.c` | `ao_append` copies each buffer to the sink and `ao_tick` hands buffers back at the speed the device plays them (only with a sink; the null path is unchanged) |

## hid shared memory layout

Confirmed on the game's own sdk by disassembling `nn::hid::GetNpadStates` (sdk+0x4a2d10): npad n state at `0x9A00 + 0x5000 * n`, style set at +0, the full key lifo at +0x28. The lifo is `u64 timestamp, u64 buffer_count (17), u64 tail, u64 count`, then 17 entries of 0x30 bytes: `u64 atomic_sampling_number` followed by the 0x28 byte `NpadFullKeyState` (`u64 sampling_number, u64 buttons, s32 lx ly rx ry, u32 attributes, u32 pad`). The SDK reads the atomic number before and after copying an entry and insists on consecutive sampling numbers in the ring, so a wrong stride makes the game spin inside `GetNpadStates` (this happened with a 0x28 stride, which is how the layout was found). Stick Y is positive up. Button bits are `nn::hid::NpadButton`.

## How it is verified

1. `python -I tests/platform/run_platform_tests.py [--game]` builds `tests/platform/test_platform.c` and runs it: hid layout through an independent reader that follows the SDK's algorithm, input mapping with synthetic SDL events, key map ini (including `config/input.default.ini`), script text, audio muted by default and the `--audio` path on the dummy driver. `--game` also runs the game hidden for 30 s, polls `EnumWindows` for the process every 250 ms (no visible window ever) and checks the log (window hidden, no audio device, surface from SDL, pad samples published).
2. Game-side proof: `hle_nn.c` logs what the game's own `nn::hid::GetNpadStates` returns whenever the buttons change (`hid read: ... buttons=0x1`). Example run: `SMRPG_INPUT="100:A:30,200:STICK_UP:40,260:PLUS:5"` shows buttons `0x1`, then `0x20000` with `lstick=(0,32767)`, then `0x400`.
3. Hidden picture: `SMRPG_CAPTURE_FRAMES=1,300` capture at present 300 is the Loading screen (2366 non-black pixels, only the spinner differs from the earlier baseline).

## Present hooks: upscale pass and overlay (task-23, stable)

`QueuePresent` (`runtime/nvn/nvn_replay.cpp`) records one command buffer per presented frame, in this order. Nothing else edits it: the overlay (task-16) and the upscaler (task-23) plug in through the two functions below.

1. Game image to the swapchain image: `upscale::Record(...)` (`runtime/gfx/upscale/upscale.h`). With `upscale = off`, the default, it returns false and the original `vkCmdBlitImage` runs.
2. `PlatOverlayDraw(const PlatOverlayFrame*)` (`runtime/gfx/upscale/overlay_hook.h`): the overlay records into the same command buffer.
3. Swapchain image to `PRESENT_SRC_KHR`, submit, present.

`PlatOverlayDraw` has a weak no-op default in `runtime/gfx/upscale/overlay_stub.cpp` (part of the NVN library). A strong definition with the same signature in any object linked into `smrpg.exe` replaces it; no `QueuePresent` edit is needed. Contract (fields are only ever appended; `struct_size` says how many the caller filled):

| Field | Meaning |
|---|---|
| `struct_size`, `frame` | `sizeof` of the caller's struct; 1-based present number of this window |
| `instance`, `physical_device`, `device`, `queue`, `queue_family` | Vulkan handles as `void*` (Vulkan 1.3, dynamic rendering and synchronization2 on). Never submit to `queue` from the hook |
| `get_instance_proc_addr` | `PFN_vkGetInstanceProcAddr`: the runtime links no Vulkan import library, so load every entry point through it (for ImGui: `ImGui_ImplVulkan_LoadFunctions`) |
| `command_buffer` | recording, outside any render pass, one-time submit; record only, never end or submit it |
| `image`, `image_view`, `image_index`, `image_count`, `format` | the swapchain image this frame presents and a 2D view of it (usage `COLOR_ATTACHMENT` and `TRANSFER_DST`); `format` is `B8G8R8A8_SRGB` when the game draws sRGB, else `B8G8R8A8_UNORM` (the sRGB case gets gamma from the view) |
| `width`, `height` | extent of `image`: the window size; the upscaled picture (any target size) already fills it |
| `layout` | `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` on entry; leave the image in it |
| `game_width`, `game_height` | size of the game's own final image |
| `swapchain_generation` | changes when the swapchain is rebuilt (resize, out of date): drop anything keyed on the old image views |

Runs on the presenting thread with the NVN lock held; do not block on the GPU. Per-frame resources need their own fences: the frame waits for its own submit before the next present (`WaitTimeline`), so a resource used only by this frame is free again at the next call.

Upscale settings (environment now, the overlay calls the `upscale::Set*` functions later): `SMRPG_UPSCALE=off|fsr1|fsr2` (default `off`), `SMRPG_UPSCALE_SHARPNESS` (RCAS attenuation in stops, default 0.25, negative turns sharpening off), `SMRPG_UPSCALE_TARGET=WxH` (output size, default the swapchain extent).

### Upscaling (task-23)

`upscale = off | fsr1 | fsr2` is applied at present time to the game's final image (`runtime/gfx/upscale/upscale.cpp`). `off` is the default and is the original blit, unchanged. `fsr1` is AMD FidelityFX FSR 1 (MIT, pinned v1.0.2, fetched by `tools/thirdparty/fetch_fsr.py`; shaders compiled at build time): EASU upscale, then RCAS sharpening, both compute passes on the encoded (gamma) values the game wrote. An sRGB game target is read through a raw UNORM view and the bytes are copied back unchanged. The output size is `SMRPG_UPSCALE_TARGET` (default the swapchain extent): equal to the swapchain it is a raw copy, otherwise a linear blit to the window. NVN's lower-left origin is honored.

`fsr2` is not available and falls back to the plain blit with a logged reason. FSR 2 needs per-pixel motion vectors, a depth buffer and camera jitter in the projection. At present time only the final color image is reachable. The game's URP 12 does not run its motion vector pass: the `Hidden/kMotion/CameraMotionVectors` and `ObjectMotionVectors` shaders are in the shader set, but the pass is off (the public ElKiro recomp forced it on). Doing it here means patching the pipeline asset in guest memory to enable the pass, finding that render target in the NVN draw stream and jittering the camera matrices. That is its own task, after the faithful path is complete. Until then `fsr1` is the upscaler.

Debug captures (all off unless set): `SMRPG_UPSCALE_CAPTURE_DIR` with `SMRPG_UPSCALE_CAPTURE_FRAMES=a,b` or `SMRPG_UPSCALE_CAPTURE_EVERY=N` writes `upscale_<frame>_in.png` (the game image) and `_out.png` (the pass output at the target size). `SMRPG_PRESENT_HASH=a,b` or `every:N` logs `present-hash: frame N ... fnv1a64 ...` of the image actually presented. With upscale off the line ends `ref-blit=equal` when the presented image equals the original plain blit redone in the same command buffer, byte for byte, and `ref-blit=DIFFERENT` otherwise. `SMRPG_PRESENT_HASH_DIR` also writes the presented image as PNG. Test: `python -I tests/upscale/run_upscale_tests.py`.

## Options overlay (task-16)

A Dear ImGui menu drawn over the presented frame. It is the one place the player changes settings; every item reads and writes `options.ini`.

Open or close it with `F1`, or with L + R + MINUS held together on a controller (scripted input: the token `F1`, for example `SMRPG_INPUT=300:F1:3`). While it is open the game gets no input: `overlay_filter_pads` (`runtime/platform/overlay/overlay_apply.cpp`) zeroes every pad before it reaches the hid shared memory, and the log says how many samples carried input (`overlay: while open, N pad samples, M carried input; all hidden from the game`). After it closes, buttons and the stick that are still held stay hidden from the game until they are let go, so closing with B does not also press B in the game. The menu is driven by the Switch button snapshot (keyboard, gamepad or script alike): d-pad or left stick up/down moves, left/right changes the value (holding repeats), A is next value, X restores the default, L/R switch page, B closes. The mouse also works while it is open (click = next value, right click = previous). Pages: Display, Performance, Audio, Controls, Menu (reset all options needs A twice; close menu).

| Page | Key in `options.ini` | Values | Applies |
|---|---|---|---|
| Display | `display.window_mode` | `windowed`, `fullscreen` | live when the window is shown (`--window`); F11 and Alt+Enter keep the option in step |
| Display | `display.upscale` | `off`, `fsr1`, `fsr2` (falls back to the blit, see above) | live (`upscale::SetMode`) |
| Display | `display.upscale_target` | `window`, `1080p`, `1440p`, `2160p` | live |
| Display | `display.sharpness` | `off`, `sharpest`, `strong`, `default`, `mild`, `soft` | live |
| Performance | `performance.fps_cap` | `game` (no cap), `30`, `60`, `90`, `120`, `144`, `240` | live (`overlay_pace`, once per presented frame, before the frame is recorded) |
| Audio | `audio.enabled` | `off`, `on` | on restart (marked in the menu) |
| Audio | `audio.volume` | 0 to 100, step 5 | live (`plat_audio_set_volume`) |
| Controls | `controls.input_map` | `auto`, or an `input*.ini` file beside the exe | live (`plat_apply_keymap`) |
| Gameplay | `gameplay.unlock_postgame` | `off`, `on` (task-24) | on save load or new game (marked "on load"); the game reads it then |
| Gameplay | `gameplay.difficulty_plus` | `off`, `hard` (task-25) | restart |

Hard uses 150% HP and 125% attack, defense, magic attack, and magic defense by default. Set the HP and damage factors on the Gameplay page or in `options.ini` and restart. To apply the separate boss factors, add `difficulty_plus_boss_ids` under `[gameplay]` with a comma-separated list of monster IDs. The same factors apply to every monster until you supply the list. The runtime serves the modified table from memory and leaves rewards at their original values.

File: `options.ini` beside `smrpg.exe`, or the path in `SMRPG_OPTIONS`. `config/options.default.ini` is the committed default (build_game.py copies it beside the exe; the generated copy is checked by `tests/platform/test_options.c`). A missing file or key keeps the default; unknown keys are preserved on save; the file is written atomically a moment after the last change, and at exit. `config/options.ini` is ignored by Git. Environment variables that already existed (`SMRPG_UPSCALE*`, `SMRPG_INPUT_CONFIG`, `--audio`) win over the file at startup; the menu then takes over.

Reading an option from game or HLE code (task-24 and later): `#include "platform/overlay/overlay.h"`, then `overlay_unlock_postgame()` (1 when `gameplay.unlock_postgame = on`) or the general `overlay_option(OPT_UNLOCK_POSTGAME)` (ids in `runtime/platform/overlay/options.h`; enum and bool options return the index, so `off`/`on` are 0/1). It reads the live value under a lock on every call, so call it when the save loads or a new game starts, not once at startup. Without the overlay build both return 0, which is the faithful default. Adding an option: one line in the table in `options.c` (section, key, page, label, help, type, default, when it applies) and an id in `options.h`; the menu page, the ini and the default file follow from the table (regenerate `config/options.default.ini` with `test_options --emit`).

The default for every option is the faithful behavior (D-004): no upscale, no cap, windowed, audio as before, the default keyboard map. With the menu closed nothing is recorded into the frame: `PlatOverlayDraw` returns on its first line, and `SMRPG_PRESENT_HASH` reports `ref-blit=equal`.

Build: Dear ImGui is not in the repo. `python tools/thirdparty/fetch_imgui.py` downloads the pinned release (v1.93.0, sha256 checked) into `A:\AI\Recomps\thirdparty\imgui` (override with `SMRPG_IMGUI_DIR`); `runtime/CMakeLists.txt` builds the overlay only when that directory exists (otherwise `overlay.h` compiles to no-ops). Threading: ImGui is touched only on the presenting thread; the main thread feeds it pad snapshots and mouse events through queues and applies the window and keymap requests the menu makes.

Contract note for the hook: the ImGui Vulkan backend uploads its font texture with its own queue submit and `vkQueueWaitIdle`, once on the first open and again when a new glyph is needed. That is one short stall while the menu is open and never happens with it closed, but it is a queue submit inside `PlatOverlayDraw` (the contract says the overlay only records). Tests: `tests/platform/test_options.c` (the options store, run by `run_platform_tests.py`).

## Heavy-slot lock (task-27)

Two leads share this PC and the owner rule is one heavy process at a time. `tools/rtprep/heavy_lock.py` is a machine-wide lock: `build_game.py` and `run_game.py` take it first, and so should any runner that launches the game (`with heavy_slot("name"):` after `sys.path.insert(0, <tools/rtprep>)`; `python -I` does not add the script directory).

- Lock file `A:\AI\Recomps\smrpg-work\heavy.lock`, JSON `{holder, pid, script, started, pid_create}`; `SMRPG_HEAVY_HOLDER` names the lead. Taken with `O_CREAT|O_EXCL`.
- A waiter prints `[heavy-lock] <script> waiting ... held by ...` every 5 s and gives up after 15 minutes (exit 75).
- A lock whose pid is dead, or alive but a different process (creation time differs), is stale and is replaced. Released in a `finally`, only if the file still names this process.
- Nested calls: the holder exports `SMRPG_HEAVY_HELD=<pid>`, so a child (for example `run_game.py` started by a test runner that holds the slot) does not wait on its ancestor. `SMRPG_HEAVY_LOCK=0` skips the lock outright; use it only for such nested calls.
- One-off command: `python -I tools/rtprep/heavy_lock.py run <name> -- <command...>`.
- Test: `python -I tests/rtprep/test_heavy_lock.py` (two real processes, stale pid, pid reuse, timeout, nested, skip, release on exception; private lock file in a temp dir).

## Known limits

- Real audio is the audout PCM path. The guest mixes its own sound and never opens an audren renderer; its Opus streams are decoded by hwopus through libopus (`runtime/audio/hwopus_dec.h`, `libopus-0.dll` beside the exe), so `--audio` carries the game's sound (task-27). The audren mixer core (`runtime/audio/audren_mix.h`) is unit-tested but not wired to RequestUpdate because no guest uses it. `SMRPG_AUDIO_DUMP=<wav>` records the audout stream offline.
- Rumble, motion, touch, handheld/joy-con styles and more than four players are not provided.
- Keyboard rebinding is an ini file; the Controls page of the overlay picks the map file (task-16).
