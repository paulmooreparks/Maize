# Maize project instructions

## The board is the source of truth

This project's backlog lives on the **maize** workbench in Andoneer
(yak.andoneer.com, via the `andoneer` MCP connector). Chat is a sidecar;
the board records what is being worked, by whom, and what stage it is in.

**Claim before you work.** Call `claim_card` (or `pull_next`) the moment
you start producing output for a card, not when you are about to move it.
An unclaimed card being worked is dark work: the operator cannot see it
in flight. If you pause or hit a blocker, `release_card` or `block_card`;
never hold a claim while idle.

## How work flows

- To work a specific card: `/work maize-N` (claims, dispatches the right
  stage subagent, and the subagent moves the card when done).
- To drive a card all the way to Done: `/fast-track maize-N`.
- To pick something up without a specific card in mind: `/pull`.
- Prefer these entry points over freehand orchestration; they carry the
  claim discipline, tier mapping, and stage handoffs procedurally.
- New work gets a card (`add_card`) before or as it starts, never after.

**Isolation is a directive you read and act on, not prose.** A column
declaring `isolation: worktree` means the dispatching session must read
that directive off the column and pass `isolation` on the Agent call that
spawns the stage subagent, on every dispatch including a continuation of
an existing session, not only the first. Passing it once and then
continuing by message is not sufficient (maize-432). If you are
dispatching freehand rather than through `/work` or `/fast-track`, this
obligation is yours directly: read the column's isolation directive
before you spawn the subagent.

Two settings-level hooks back this up in this repository
(`.claude/settings.json`, `scripts/hooks/`): one denies destructive git
commands run outside `.claude/worktrees/`, the other records the working
directory of every shell call so a lapse is measurable rather than found
by accident. Neither is a substitute for passing isolation correctly;
they exist because passing it correctly has failed three times already.

## Conventions

- Commits for card work carry the `maize-NN: ` prefix and push to
  `origin/master` (trunk; no integration branch).
- Handoff notes follow the pointer-note protocol: full report as a card
  comment, short note with the required headings plus the comment link.
- No em-dashes and no smart quotes in committed text, card comments, or
  handoff notes.
