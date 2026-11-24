---
id: task-0
title: Project work board (shared Artifact)
status: Ready
---
## Problem before solution
The owner wants to see at a glance what is planned, in progress and done, the same way as their existing work board (artifact db collection `items`, columns idea, planned, in_work, revision, bug, finished).
## Outcome and scope
A Super Mario RPG PC work board artifact: same mechanics as that board, retitled and restyled for this project, with a column for "waiting on the owner" visible, each card showing task id, owner subchat, model and effort. Seeded with task-0 to task-7 from `backlog/tasks/`. Page source in `tools/board/work-board.html`. Non-goals: syncing automatically with the markdown cards (the coordinator updates both).
## Dependencies and ownership
None. Owns `tools/board/`.
Execution: subchat, Sonnet, medium effort.
## Acceptance and evidence
| ID | Criterion | Scenario/assertion | Required evidence | Result |
|---|---|---|---|---|
| AC1 | Artifact published and opens; cards load from its db | Open link | Link and ArtifactData list output | NOT_RUN |
| AC2 | Eight seeded cards in the right columns | ArtifactData list | Output | NOT_RUN |
## Human gates
The owner's look approval. Pinning only on the owner's yes.
## Recovery and return condition
Return the artifact URL and db schema at done.
## Approach and commands
Load artifact-design and artifact-capabilities skills first. Read the existing board page as the model.
## Log and handoff
Record URL, collection and field names.
