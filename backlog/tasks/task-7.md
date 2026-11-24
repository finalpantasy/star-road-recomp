---
id: task-7
title: Research bugs players still report in 1.0.1 (fix list for the port)
status: Verified
---
## Problem before solution
The owner wants the PC version to fix bugs that remain in Switch 1.0.1. Nobody has collected them yet.
## Outcome and scope
`docs/analysis/known-bugs-1.0.1.md`: a table of reported bugs (gameplay, softlocks, text/translation, audio, visual, performance such as frame drops and load times), each with: description, where in game, how to reproduce, source links (Reddit, GameFAQs, ResetEra, speedrun.com / speedrun Discord notes, YouTube, Nintendo support), whether it is confirmed on 1.0.1 (not just 1.0.0), how many independent reports, and a first guess at fix category (game logic, rendering, timing, data). Also list the 1.0.1 patch notes so fixed issues are excluded. Non-goals: fixing anything; cheats or glitches speedrunners rely on are listed separately as "keep as option", not as bugs.
## Dependencies and ownership
None. Owns only `docs/analysis/known-bugs-1.0.1.md`.
Execution: subchat, Sonnet, medium effort (web research).
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Each bug has at least one working source link and a 1.0.1 status (confirmed, likely, fixed, unknown) | Doc review | Doc | PASS (lead review 2026-11-24) |
| AC2 | Official 1.0.1 patch notes quoted by link and summarized | Doc review | Doc | PASS (lead review 2026-11-24) |
| AC3 | Ranked top 10 by player impact | Doc review | Doc | PASS (lead review 2026-11-24) |
## Human gates
The owner picks which bugs to fix and which speedrun glitches stay as options.
## Recovery and return condition
Doc only. Return at done.
## Approach and commands
Web search and page reads only. No posting, sign-ins or downloads. Short quotes only (under 15 words), summarize in own words.
## Log and handoff
List sources searched and those that failed.
