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

## Known limits

- Real audio is only the audout PCM path. The game's music and effects go through audren (`hle_audio.c` has no DSP: a null renderer), so `--audio` is silent until an audren mixer exists. That is a separate task.
- Rumble, motion, touch, handheld/joy-con styles and more than four players are not provided.
- Keyboard rebinding is an ini file; a UI is the overlay's (task-16).
