# Star Road: Super Mario RPG PC Recompilation

A native Windows PC version of **Super Mario RPG** (Nintendo Switch, 2023, version 1.0.1), built from your own legally dumped copy of the game.

The plan has two stages:

1. **Static recompilation.** Our own ahead-of-time recompiler translates the game's ARM64 code to native x86-64. Switch system services and the NVN graphics API are reimplemented on Windows and Vulkan. No emulator, no JIT.
2. **Full decompilation.** The game logic is rewritten as readable source on a lightweight native engine. Each rewritten function is swapped into the stage 1 build and checked against the original, so the decomp stays faithful.

Planned upgrades, each with an option to turn it off: 4K and higher internal resolution, high frame rate, quality-of-life options (faster text, battle speed, remappable controls) and mod support. Fixes for bugs still present in 1.0.1 are on the list too.

## Status

Early. Analysis and tooling only, nothing playable yet.

- Extraction from your dump: working (`tools/extract.py`).
- Function naming from the game's IL2CPP metadata: in progress.
- Platform survey (graphics, audio, video, saves): in progress.
- Recompiler: being designed.

Screenshots will go here once the game boots.

## What this repository contains

Only original code: tools, the recompiler, the runtime and, later, the engine. It contains **no game code, assets, keys or data**. You need:

- your own Super Mario RPG cartridge or eShop dump (base game XCI or NSP, plus the 1.0.1 update),
- your own `prod.keys` from your own Switch.

Generated code and extracted files stay on your machine and are rebuilt from your dump.

## Extract your dump

```
python -I tools/extract.py --dumps <folder with XCI and NSP> --keys <path to prod.keys> --hactool <path to hactool.exe> --out <work folder>
```

The script checks every output against known hashes in `tools/inputs.json`.

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Nintendo. Super Mario RPG is a trademark of Nintendo. Do not open issues asking for game files or keys.

## License

MIT for the code in this repository. See `LICENSE`.
