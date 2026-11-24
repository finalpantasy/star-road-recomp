# Super Mario RPG PC
Player fantasy/audience: the owner's first RPG, played natively on PC at its best: the Switch remake exactly as designed, plus 4K, high frame rate, quality-of-life options and mods. Audience: the owner first; public tooling later (D-005).
Main verb and short loop: explore, talk, fight timed-hit turn battles, level up, advance the story. The port must preserve every loop exactly, including timed-hit windows.
Meaningful choices and intended feeling: the original's charm and responsiveness. Input latency and timed hits must feel at least as good as Switch.
Reference lessons and differentiation:
- Zelda64Recomp / N64Recomp: static recomp can feel native and enable upgrades; ship tools, never game code.
- UnleashedRecomp / XenonRecomp: high-FPS and resolution upgrades need logic fixes where the game assumed a fixed rate.
- New Pokemon Snap native PC (the owner's own): Switch HLE plus NVN to Vulkan works; reference tests per subsystem kept it honest.
- Decomp projects (sm64, OoT): a real decomp enables mods and a clean engine. Here IL2CPP metadata gives us every C# name, which no N64 decomp had.
First playable and success/failure/restart: Milestone M1, the recompiled game boots to the title screen on PC. M2: new game through the first battle with correct timed hits. M3: full playthrough parity. Then the decomp.
Platform/input/hardware: Windows 11 x64, Vulkan GPU, XInput/SDL controller plus keyboard. Linux later.
Non-goals for now: online features, ultrawide focus (the owner: "not so much"), Android, redistribution of any game data.
Experience hypothesis and disproof: a recompiled build plays identically to Switch 1.0.1 (state hashes match on recorded input routes) while running at 4K and high frame rate. Disproved if any route diverges or timed hits shift with frame rate.
