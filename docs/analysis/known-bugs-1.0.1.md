# Known bugs in Super Mario RPG (Switch, 2023) version 1.0.1

Task: task-7. Research date: 2026-11-24. Method: web search plus page reads only. No posting, sign-ins or downloads.

## Bottom line

Very little is still reported against 1.0.1. The only patch ever found is 1.0.1, and it fixed the serious softlocks. After that patch I found no dated, version-tagged, independently confirmed bug report. What remains is a short list of low-confidence player reports, one performance weakness (frame-time hitches), and a few exploits that survive and should be options. Reddit, GameFAQs, ResetEra, speedrun.com and YouTube could not be read (see Sources failed), so this list is probably incomplete. Treat the "unknown" rows as leads to check against the real game logic once the decomp exists, not as proven bugs.

## 1. Official 1.0.1 patch notes (AC2)

Released December 12, 2023 (Dec 13 in Europe and Japan coverage). Nintendo announced the underlying bug on Nov 22, 2023 and promised a December fix.

Official notice (Nintendo AU): https://nintendo.com/au/support/articles/super-mario-rpg-notice-regarding-a-bug
Full notes as reprinted by press: https://www.nintendolife.com/news/2023/12/super-mario-rpg-version-1-0-1-released-here-are-the-full-patch-notes
Also: https://mynintendonews.com/2023/12/13/super-mario-rpg-updated-to-version-1-0-1-patch-notes/ , https://finalweapon.net/2023/12/12/super-mario-rpg-update-1-0-1-now-live-with-patch-notes/ , https://www.mariowiki.com/Super_Mario_RPG_(Nintendo_Switch) (Update history section lists only 1.0.1).

I did not find the notes on a Nintendo support page of their own, only the AU bug notice plus press copies. Summary in my own words, seven fixes, six of them progression blockers:

| # | Fixed in 1.0.1 | Type |
|---|---|---|
| F1 | Paratroopas now appear at Land's End after the Monstro Town Paratroopa event | Softlock |
| F2 | Battle no longer restarts in a loop right after an enemy fight in the Mushroom Kingdom | Softlock |
| F3 | Mario no longer stays in the "surprised" state during a Marrymore event | Softlock |
| F4 | Bowser's Keep six-door section: doors 5 and 6 can be entered again | Softlock |
| F5 | Mario no longer stays surprised in the Sunken Ship cannon area | Softlock |
| F6 | Screen now changes after stomping a Shogun in Land's End | Softlock |
| F7 | Thought Peek messages for Straw Head and Reacher were swapped, now correct | Text |

These are excluded from the bug table below. For the port, note that a faithful 1.0.1 build must already contain these fixes, so any recompiled or decompiled logic should be checked to confirm the fixed behavior (see Test ideas).

## 2. Bug table (AC1)

Status key: confirmed (stated on 1.0.1 or after the patch, 2+ independent reports), likely (post-patch reports, thin), unknown (version not stated or single report), fixed (excluded, see section 1). Fix category is a first guess only.

| ID | Description | Where in game | Repro | Sources | 1.0.1 status | Independent reports | Fix category (guess) |
|---|---|---|---|---|---|---|---|
| B1 | Frame-time hitches while moving through some maps and in some battles. Target is 60 fps and it holds almost always, but drops are visible when they happen. Intro scene is named as worst. | Intro scene, some field maps, some village areas | Play on Switch, docked or handheld | https://mynintendonews.com/2023/11/16/digital-foundry-examines-super-mario-rpg-says-its-almost-perfect , https://www.gamerevolution.com/guides/955668-super-mario-rpg-remake-performance-docked-handheld , https://nintendoeverything.com/?p=820999 | likely (Digital Foundry tested at launch on 1.0.0; Nintendo Life commenters mention it after 1.0.1 without scenes or versions) | 3+ outlets, plus commenters | Rendering, timing (largely a Switch hardware limit; PC should not inherit it) |
| B2 | Frog Sage at Tadpole Pond keeps repeating the same lines and will not advance, blocking the way to Star Hill. Not in the 1.0.1 notes. May be player error. | Tadpole Pond | Unknown | https://samurai-gamers.com/super-mario-rpg-remake/bug-report-board-11/ (Nov 19 and Nov 21, 2023) | unknown (no version stated, both reports predate or are near the patch) | 2 | Game logic (event flag) |
| B3 | Grandpa Frog stays in his corner after Peach joins and says it will be quiet without Mallow. Reporters doubted it was a bug; may be an unmet story trigger. | Frog Pond area | Unknown | https://samurai-gamers.com/super-mario-rpg-remake/bug-report-board-11/ (Nov 27 and Nov 29, 2023) | unknown | 2 | Game logic (event flag), maybe not a bug |
| B4 | Peach's Group Hug prompt: the exclamation mark does not appear, or pressing A does nothing. | Battle, Peach special | Unknown | same board (Nov 19, 2023) | unknown | 1 | Game logic or input timing |
| B5 | 100-jumps challenge reported harder in docked mode than handheld, suspected input latency. Speculation only. | Jump challenge (Booster Tower area) | Compare docked and handheld | Nintendo Life comments on the 1.0.1 notes article (link in section 1) | unknown | a few commenters | Timing, input latency |
| B6 | One commenter said 1.0.1 did not fix their progression problem and the Mushroom Kingdom workaround failed. No details, cannot be tied to a specific bug. | Unspecified | Unknown | Nintendo Life comments (link in section 1) | unknown | 1 | Unknown |

Not bugs (deliberate changes, do not "fix"): Frogfucius renamed in the remake, slower Rose Town gardener text scroll (a complaint, not an error), original exploits removed on purpose.

Leads I could not check: translation or text errors specific to the remake (search found none documented), audio glitches (none found), long load times (reviews say loads are short, so no report), crashes (none found for this game; general Switch 2 crash threads exist but none name this game).

### 2b. Second pass additions (YouTube, speedrun wiki, GitHub)

YouTube search pages and the community speedrun wiki were readable; Reddit, speedrun.com and ResetEra were not (see section 6). These are video-title leads, not verified bugs. I could not open the videos, so none has a version number or repro steps.

| ID | Description | Where in game | Sources | 1.0.1 status | Independent reports | Fix category (guess) |
|---|---|---|---|---|---|---|
| B7 | "Softlocked" Short about a Shy Guy loop in a fight. Titles only; may be the pre-patch battle-restart loop (F2). | Unknown | YouTube search results page: https://www.youtube.com/results?search_query=super+mario+rpg+remake+glitch+bug+softlock (video by SuperMonkeyCherrim, about 3 years old) | unknown, likely pre-1.0.1 | 1 | Game logic |
| B8 | Short titled "Super Mario RPG Masher Bug" (68 views). Title only. | Unknown | same search results | unknown | 1 | Unknown |
| B9 | Shorts titled "I found a glitch in smrpg switch" and "Accidental Glitch in Mario RPG?!" (about 60 views each). Titles only. | Unknown | same search results | unknown | 2 (separate uploads) | Unknown |
| B10 | "5 Easy Glitches You Can Do for Super Mario RPG Remake" by The Gaming Branch (2K views, about 2 years old). Title only; contents unknown. | Unknown | same search results | unknown | 1 | Unknown |

GitHub issue searches for the Switch title ID 0100BC0018138000 and "Super Mario RPG" returned only an Android emulator startup report (a compatibility issue, emulator-only, game version 1.0.0, https://github.com/orikami-dev/Masu issue 8), a Nintendo Switch mod thread that lists a v1.0.1 4:3 aspect ratio patch, and tools that merely list the title. No Ryujinx, Citron or Eden game bug report surfaced (the Eden tracker search URL returned 404). Emulator-only bugs, if any exist, are out of scope for the port.

## 3. Speedrun glitches and exploits: "keep as option"

These are not bugs for this list. Per the card, the owner decides which stay.

| ID | Item | Status in remake | Source | Note |
|---|---|---|---|---|
| S1 | Geno Whirl kills Exor in one hit once an eye is down (9999 damage on a critical) | Still works in the remake, even though many other old exploits were removed | https://www.destructoid.com/can-you-still-one-shot-exor-in-super-mario-rpg/ , https://gamesradar.com/a-27-year-old-jrpg-bug-is-now-a-feature-in-the-super-mario-rpg-remake (headline and snippet only) | Fans suspect an oversight, but Nintendo kept it. Suggest default ON (faithful), no option needed unless the owner wants one. |
| S2 | Skill Swap, Mack Skip and other original-game exploits | Reported removed or patched in the remake | https://game8.co/articles/featured/super-mario-rpg-remakes-crushes-bugs-and-speedrunning-potential (via search snippets only; page returned 403) | Do not recreate in the faithful mode. |
| S3 | Remake-specific skips, sequence breaks and fast-travel tricks | They exist. An Abyssoft video (about 162K views, chapters "Skipping", "Sequence Breaks", "Fast Travel") says runners found many skips and breaks; GDQ and RPG Limit Break ran the remake in about 2:29 to 2:38, with a separate 3D Culex fight at the end. Details of each trick were not readable (video descriptions only). | https://www.youtube.com/results?search_query=Abyssoft+Speedrunners+Have+Already+Broken+the+SMRPG+Remake , community wiki https://sites.google.com/site/supermariorpgspeedruns/switch (route pages returned 404) | Needs a person to read speedrun.com rules and the Discord to list each trick. Default: keep all as faithful behavior, no fixes. |

## 4. Ranked top 10 by player impact (AC3)

The evidence supports fewer than ten real post-1.0.1 bugs. Ranks 1 to 6 are the real candidates; 7 to 10 are fixed-in-1.0.1 items that matter to the port as regression tests, ranked by how bad they were. Each line says whether it is a candidate fix or a test.

1. B1 Frame-time hitches (intro, some maps and battles). Most widely reported; PC engine should remove it. Candidate: rendering, timing.
2. B2 Frog Sage dialogue loop at Tadpole Pond (blocks progress if real). Verify against event flags in the decomp.
3. B6 Unspecified progression block that survived 1.0.1 (single anonymous claim). Watch for more evidence.
4. B5 Docked versus handheld jump-challenge difficulty (possible input latency). PC should measure input latency in the faithful mode.
5. B4 Peach Group Hug prompt sometimes missing or ignored.
6. B3 Grandpa Frog stuck after Peach joins (low impact, may not be a bug).
7. F1 Land's End Paratroopa softlock (fixed; regression test).
8. F2 and F6 Mushroom Kingdom battle restart loop and Land's End Shogun screen change (fixed; regression tests).
9. F3, F5 Marrymore and Sunken Ship "surprised Mario" blocks (fixed; regression tests).
10. F4 Bowser's Keep doors 5 and 6 (fixed; regression test).

Second pass check: the YouTube leads B7 to B10 are title-only with no version or repro, so they do not enter the top 10. The ranking above stands. Re-rank once Reddit and speedrun.com can be read.

## 5. Test ideas for the port

- Replay the seven 1.0.1 fix scenarios (F1 to F7) as reference tests against the recompiled build: each must progress normally.
- Event-flag audit for the Frog Pond and Tadpole Pond scenes (B2, B3) once the script and flag data are decoded.
- Input-to-screen latency capture in a faithful mode for B5.

## 6. Sources searched and failed

Searched and used: Nintendo AU support notice, Nintendo Life, Nintendo Everything, MyNintendoNews, GameRevolution, Gameranx, Destructoid, GamesRadar (headline and snippets only), Super Mario Wiki (remake page; glitch list covers only the SNES original), Samurai Gamers bug board, assorted news sites.

Second pass (agent browser, built-in browser, GitHub search): the Game8 article body (read in full), YouTube search pages, the community speedrun wiki Switch page, GitHub issue search all worked.

Failed or unusable:
- Reddit (r/MarioRPG, r/NintendoSwitch, r/speedrun): still blocked. The reddit-reader tool cannot search (thread URLs only), and the agent browser's Reddit login looks lapsed (the page said "blocked by network security"). Largest gap in this report. The owner would need to run `python ~/.claude/skills/agent-browser/agent_browser.py login` and sign in themselves, and someone must supply thread URLs.
- GameFAQs: the boards URL I tried opened an unrelated game page; the remake boards were not found.
- ResetEra and speedrun.com: Cloudflare bot check ("Just a moment"), unreadable. speedrun.com/smrpg_remake returned "not found".
- Ryujinx, Citron, Eden trackers: no Super Mario RPG bug issue found (Eden search URL 404; Ryujinx is archived).
- speedrun wiki route and video pages: 404.
- YouTube video pages: only search-result titles, descriptions and chapters were readable.
- speedrun.com (game page and rules, first pass): HTTP 403.
- Game8 remake article: HTTP 403 (used search snippets only).
- GamesRadar body: truncated.
- YouTube: not readable through these tools.
- Super Mario Wiki remake page: first 100,000 of about 180,000 characters read; no glitch section found.
- A wiki talk page returned text aimed at AI agents. It was ignored as untrusted data.

Suggested next step for the owner: renew the agent-browser Reddit login, supply or collect r/MarioRPG thread URLs for bugs, and have a person read speedrun.com rules and the speedrun Discord. This worker does not sign in.
