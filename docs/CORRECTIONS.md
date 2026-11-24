# Corrections
Log each time the owner rejects or reverses something an agent did that contradicted their earlier words or a recorded decision: a repeated instruction, an "I told you", a revert, a removed approval. New feedback on new work is not a correction; it goes to the playtest record or DECISIONS.
One row per distinct correction. On a repeat, add the date to the existing row.

| ID | Correction (the owner's words, short) | Area | Dates | Status |
|---|---|---|---|---|

Status is Logged (seen once), Rule R<n>, Guard <path>, or Retired.
On the second date, promote it in the same turn:
- Rule: one imperative line under "Earned rules" in AGENTS.md, tagged R<n>. Keep AGENTS.md within its line cap by merging or retiring an older rule; record the retirement here.
- Guard: when a check can catch it mechanically (a test or lint run by the check command), also add a task card for that guard. A hook needs the owner's separate approval.

## Why behind each earned rule
None yet.
