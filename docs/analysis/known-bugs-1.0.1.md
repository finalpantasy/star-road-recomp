# Known bugs in Super Mario RPG (Switch, 2023) version 1.0.1

Scope: only the Nintendo Switch remake at version 1.0.1. Reports about the SNES original or SNES emulators are excluded.

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
| B1 | Frame-time hitches and a stuttering camera on Switch 1. Target is 60 fps and it holds most of the time. Reddit users name Mushroom Kingdom (town square, fireplaces), Kerokero Sewers water, Rose Way, the Sea (whirlpool shader), Marrymore and Bowser's Castle cutscenes. One commenter says some cutscenes fall to 30 fps (double-buffered vsync). | Field maps and cutscenes listed at left; intro scene per Digital Foundry | Play on Switch 1, docked or handheld | https://www.reddit.com/r/MarioRPG/comments/17tcafp/ (Nov 2023, launch build) , https://www.reddit.com/r/MarioRPG/comments/1l3uixx/ (about June 2025, Switch 2 owner says drops and camera stutter vanished; several commenters recall the Switch 1 drops, a larger group says they never noticed), https://mynintendonews.com/2023/11/16/digital-foundry-examines-super-mario-rpg-says-its-almost-perfect , https://nintendoeverything.com/?p=820999 | confirmed on 1.0.1 (the 2025 thread is long after the only patch; no later patch exists) | 3+ outlets and about 5 Reddit commenters across 2 threads; disputed by others | Rendering, timing (largely a Switch 1 hardware limit; PC should not inherit it) |
| B2 | Pseudo-softlock, not a code bug: after Peach joins, the Frog Sage only says he misses Mallow and nothing advances. The story needs the Cricket Pie from the Mushroom Kingdom item shop. Players keep reading it as a bug. | Tadpole Pond, after Marrymore | Reach Peach joining without buying the Cricket Pie, then talk to the Frog Sage | https://www.reddit.com/r/MarioRPG/comments/1acvxeg/ , https://www.reddit.com/r/MarioRPG/comments/18qwb9w/ , https://www.reddit.com/r/MarioRPG/comments/1831z92/ , https://www.reddit.com/r/MarioRPG/comments/18awf9t/ , https://www.reddit.com/r/MarioRPG/comments/1qmdm12/ , https://www.reddit.com/r/MarioRPG/comments/1y0p413/ (a Switch 2 player, within the last weeks), https://samurai-gamers.com/super-mario-rpg-remake/bug-report-board-11/ | confirmed as intended behavior on 1.0.1 (answers in every thread name the Cricket Pie) | 8+ independent threads, Nov 2023 through 2026 | Data or UX (no code fix needed; candidate "hint" option, off by default) |
| B3 | Grandpa Frog stays in his corner after Peach joins and says it will be quiet without Mallow. This is the same Cricket Pie trap as B2, so it is not a separate bug. | Frog Pond area | see B2 | same threads as B2 | merged into B2 | merged | n/a |
| B4 | Peach's Group Hug prompt: the exclamation mark does not appear, or pressing A does nothing. | Battle, Peach special | Unknown | same board (Nov 19, 2023) | unknown | 1 | Game logic or input timing |
| B5 | Input latency complaints: 100-jump challenge and timed hits feel off on wireless pads (8bitdo, Pro controller) and docked, fine with Joy-Cons attached or a wire. Player-side lag is the likely cause, but the game's own timing may add to it. | Super Jump challenge, timed attacks | Compare wireless pad on a TV with attached Joy-Cons | https://www.reddit.com/r/MarioRPG/comments/181hakx/ , https://www.reddit.com/r/MarioRPG/comments/1828a1d/ (both Nov 2023), Nintendo Life comments on the 1.0.1 notes article (link in section 1) | likely (reports straddle the patch date; no fix claimed) | about 4 | Timing, input latency |
| B6 | A Nintendo Life commenter said 1.0.1 did not fix their progression problem. Reddit explains the likely cause: the Land's End Paratroopas also need the Monstro Town Monster Mama dialogue first. A June 2024 poster said the Mushroom Kingdom workaround failed and that they were unsure the game was updated; talking to Monster Mama fixed it. Likely player error, not a leftover bug. | Land's End cliff | Skip Monster Mama's talk in Monstro Town, go to the cliff | https://www.reddit.com/r/MarioRPG/comments/1dib94l/ (about June 2024), Nintendo Life comments (link in section 1) | likely not a bug (resolved without any fix) | 2 | Game logic (trigger order) or UX |

Not bugs (deliberate changes, do not "fix"): Frogfucius renamed in the remake, slower Rose Town gardener text scroll (a complaint, not an error), original exploits removed on purpose.

Leads I could not check: translation or text errors specific to the remake (search found none documented), audio glitches (none found), long load times (reviews say loads are short, so no report), crashes (none found for this game; general Switch 2 crash threads exist but none name this game).

### 2b. Second pass additions (YouTube, speedrun wiki, GitHub)

YouTube search pages and the community speedrun wiki were readable in the second pass (Reddit came in the third, see 2c; speedrun.com and ResetEra are still blocked, see section 6). These are video-title leads, not verified bugs. I could not open the videos, so none has a version number or repro steps.

| ID | Description | Where in game | Sources | 1.0.1 status | Independent reports | Fix category (guess) |
|---|---|---|---|---|---|---|
| B7 | "Softlocked" Short about a Shy Guy loop in a fight. Titles only; may be the pre-patch battle-restart loop (F2). | Unknown | YouTube search results page: https://www.youtube.com/results?search_query=super+mario+rpg+remake+glitch+bug+softlock (video by SuperMonkeyCherrim, about 3 years old) | unknown, likely pre-1.0.1 | 1 | Game logic |
| B8 | Short titled "Super Mario RPG Masher Bug" (68 views). Title only. | Unknown | same search results | unknown | 1 | Unknown |
| B9 | Shorts titled "I found a glitch in smrpg switch" and "Accidental Glitch in Mario RPG?!" (about 60 views each). Titles only. | Unknown | same search results | unknown | 2 (separate uploads) | Unknown |
| B10 | "5 Easy Glitches You Can Do for Super Mario RPG Remake" by The Gaming Branch (2K views, about 2 years old). Title only; contents unknown. | Unknown | same search results | unknown | 1 | Unknown |

GitHub issue searches for the Switch title ID 0100BC0018138000 and "Super Mario RPG" returned only an Android emulator startup report (a compatibility issue, emulator-only, game version 1.0.0, https://github.com/orikami-dev/Masu issue 8), a Nintendo Switch mod thread that lists a v1.0.1 4:3 aspect ratio patch, and tools that merely list the title. No Ryujinx, Citron or Eden game bug report surfaced (the Eden tracker search URL returned 404). Emulator-only bugs, if any exist, are out of scope for the port.

### 2c. Reddit pass (r/MarioRPG, r/NintendoSwitch, r/Mario, read-only)

I read about 30 threads and searched for bug, glitch, softlock, crash, freeze, frame rate and Switch 2 terms. Most "bug" posts turned out to be player error. Posts about the SNES original or SNES emulators are out of scope (the port is the Switch remake) and are dropped from this list. Thread base: https://www.reddit.com/r/MarioRPG/comments/ plus the id shown. Dates are approximate (from thread ids).

Candidates (possible real bugs or oddities in the Switch remake):

| ID | Description | Where in game | Repro | Source id | 1.0.1 status | Reports | Verdict | Fix category (guess) |
|---|---|---|---|---|---|---|---|---|
| B11 | Super Suit given after 99 jumps instead of 100. The poster failed at 99 twice, the K-9 still showed a record of 99, then on the second try the suit was granted anyway. Commenters guess an off-by-one or a lenient count. | Booster Tower Super Jump challenge | Reach 99 consecutive jumps, miss the last | 193k9vv (about Jan 2024) | likely on 1.0.1 (after the patch) | 1 (seven commenters, none reproduced) | Possible real quirk, unconfirmed | Game logic |
| B12 | Belome dialogue plays out of order when the fight starts from an odd angle (the trigger still forces Mario in front of Belome, but the line order is wrong). | Belome Temple | Start the fight by jumping on his head or from the side | 1ji4ath (about Mar 2025) | likely on 1.0.1 | 1 | Possible real minor bug | Game logic (event trigger) |
| B13 | Peach joins, Frog Sage and Land's End triggers depend on hidden prior steps (Cricket Pie, Monster Mama). See B2 and B6. | n/a | n/a | multiple | n/a | n/a | Design trap, not a bug | UX option |

Checked and ruled out (player error). Threads about the SNES original or SNES emulators (invisible-enemy square, Countdown model on bsnes, Kipper water-walk, missing-chest message) were dropped as out of scope: Sunken Ship Rematch "not working" (needs the Cricket Pie step, 18awf9t), Frying Pan "low damage" (the Lazy Shell lowers attack, 1917m3g), "Impossible beanstalk" (a hidden moving platform appears if you jump straight up, 1uc4cjg), "Game keeps freezing" and opening-cutscene crash (a reinstall or reseating the cartridge fixed it; a download glitch is the likely cause, 1bp8ese and 18eteg7), ball solitaire prompt missing (only the correct ball can be kicked, 1fxc02t and 1tn10wh), "progress not saved" after the credits (intended post-game state, 1bhbbkr), Booster rematch "bugged" (the Snifit speed mechanic makes him act twice; two posters still suspect RNG, 17z5rss, Nov 2023), 101-jump mission (just difficulty, 1w6r6x0).

Pre-patch items confirmed as the F1 to F7 list (not new): Paratroopa missing at the cliff (17yxlsq, 17yetow, 1801e6z, 17yyhrv), the Shy Guy battle loop (17zt271, 17yyj47), plus posts about Land's End. The 1.0.1 announcement thread (18h7a4v) has no leftover-bug reports.

Speedrun and exploit mentions from Reddit: Geno Whirl on Exor works in the remake (18zh9hu, 1f9wzhb), confirming S1.

Not found on Reddit: no crash after 1.0.1 beyond the reinstall cases above, no audio glitch report, no text or translation error report, no long-load report apart from one vague comment (a Switch 1 owner mentioning "long load times and other graphical glitches" in 1l3uixx). Nothing about a Switch 2 compatibility fault for this game; the r/NintendoSwitch Switch 2 compatibility thread lists it as 60 fps.

## 3. Speedrun glitches and exploits: "keep as option"

These are not bugs for this list. Per the card, the owner decides which stay.

| ID | Item | Status in remake | Source | Note |
|---|---|---|---|---|
| S1 | Geno Whirl kills Exor in one hit once an eye is down (9999 damage on a critical) | Still works in the remake, even though many other old exploits were removed | https://www.destructoid.com/can-you-still-one-shot-exor-in-super-mario-rpg/ , https://gamesradar.com/a-27-year-old-jrpg-bug-is-now-a-feature-in-the-super-mario-rpg-remake (headline and snippet only) | Fans suspect an oversight, but Nintendo kept it. Suggest default ON (faithful), no option needed unless the owner wants one. |
| S2 | Skill Swap, Mack Skip and other original-game exploits | Removed in the remake, so nothing to keep; listed only to show they are out of scope | https://game8.co/articles/featured/super-mario-rpg-remakes-crushes-bugs-and-speedrunning-potential (via search snippets only; page returned 403) | Do not recreate in the faithful mode. |
| S3 | Remake-specific skips, sequence breaks and fast-travel tricks | They exist. An Abyssoft video (about 162K views, chapters "Skipping", "Sequence Breaks", "Fast Travel") says runners found many skips and breaks; GDQ and RPG Limit Break ran the remake in about 2:29 to 2:38, with a separate 3D Culex fight at the end. Details of each trick were not readable (video descriptions only). | https://www.youtube.com/results?search_query=Abyssoft+Speedrunners+Have+Already+Broken+the+SMRPG+Remake , community wiki https://sites.google.com/site/supermariorpgspeedruns/switch (route pages returned 404) | Needs a person to read speedrun.com rules and the Discord to list each trick. Default: keep all as faithful behavior, no fixes. |

## 4. Ranked top 10 by player impact (AC3)

The evidence supports few real post-1.0.1 code bugs. Ranks 1 to 7 are open items or player-impact traps; 8 to 10 are fixed-in-1.0.1 groups that matter to the port as regression tests. Each line says whether it is a candidate fix, an option, or a test. Re-ranked after the Reddit pass.

1. B1 Frame-time hitches and camera stutter on Switch 1 (confirmed on 1.0.1, several reports). PC engine should remove it. Candidate: rendering, timing.
2. B2 Cricket Pie trap at the Frog Sage (8+ threads, still hitting new players in 2026). Not a code bug; candidate optional hint, default off.
3. B6 Land's End cliff Paratroopas still "missing" when Monster Mama is skipped (player error after 1.0.1). Same hint option as B2.
4. B5 Input latency on wireless pads and docked, hurting the 100-jump challenge. Measure latency in the faithful mode; a low-latency input option.
5. B11 Super Suit granted at 99 jumps (1 report, unconfirmed). Verify the counter in the decomp.
6. B12 Belome dialogue out of order (1 report). Verify the trigger in the decomp.
7. B4 Peach Group Hug prompt sometimes missing (1 report, no version).
8. F1 and F6 Land's End Paratroopa and Shogun softlocks (fixed; regression tests).
9. F2, F3 and F5 Mushroom Kingdom battle loop, Marrymore and Sunken Ship "surprised Mario" blocks (fixed; regression tests).
10. F4 Bowser's Keep doors 5 and 6 (fixed; regression test).

The YouTube leads B7 to B10 are title-only with no version or repro, so they do not enter the top 10.

## 5. Test ideas for the port

- Replay the seven 1.0.1 fix scenarios (F1 to F7) as reference tests against the recompiled build: each must progress normally.
- Event-flag audit for the Frog Pond, Tadpole Pond and Land's End cliff scenes (B2, B6) once the script and flag data are decoded; confirm the Cricket Pie and Monster Mama dependencies.
- Input-to-screen latency capture in a faithful mode for B5.
- Super Jump counter test for B11 (does a miss at 99 grant the suit?) and Belome trigger test for B12.
- Frame-time capture of the B1 scenes on the PC build for comparison.

## 6. Sources searched and failed

Searched and used: Nintendo AU support notice, Nintendo Life, Nintendo Everything, MyNintendoNews, GameRevolution, Gameranx, Destructoid, GamesRadar (headline and snippets only), Super Mario Wiki (remake page; its glitch list covers only the SNES game, which is out of scope), Samurai Gamers bug board, assorted news sites.

Second pass (agent browser, built-in browser, GitHub search): the Game8 article body (read in full), YouTube search pages, the community speedrun wiki Switch page, GitHub issue search all worked.

Failed or unusable:
- Reddit: readable in the third pass after the owner renewed the login (about 30 threads in r/MarioRPG, r/NintendoSwitch and r/Mario, via the search pages and the reddit-reader tool). The Reddit search shows only a limited set of results per query, so some threads were not seen. r/speedrun was not searched. Comment sections were cut at 20 to 30 comments per thread.
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

Suggested next step for the owner: have a person read speedrun.com rules and the speedrun Discord, and decide which of B2, B5, B11 and B12 to handle (hint option, latency option, counter and trigger checks). This worker does not sign in.
