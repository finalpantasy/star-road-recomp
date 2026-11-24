# Reuse ledger (D-009)

Reused code lives only in the gitignored `reuse/` folder (and on A:). It is never committed or pushed. Each row is removed when our own rewrite replaces it and passes the same tests with the reused version as the oracle. Public release requires this table to be empty.

| Piece | Source (project, commit, path) | Origin license | Used by | Rewrite card | Status |
|---|---|---|---|---|---|
