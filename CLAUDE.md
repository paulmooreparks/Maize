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
before you spawn the subagent. Pass the column's other execution-shaping
directives in the same breath, because a subagent that cannot see one
will infer it from the filesystem, silently and plausibly.

**On this repository, passing the directive is necessary and not
sufficient.** The harness currently refuses to create the worktree, on
every dispatch, so the dispatcher creates it by hand with `git worktree
add` and names its absolute path in the brief. Tell the subagent that
starting in the operator's checkout is expected rather than fatal, and
that it should `cd` to the named path first: a brief that reads as a
pre-flight gate will make an agent block instead of working. Have it
verify its location equals the one named path, never merely that the
path contains `.claude/worktrees/`, because an agent has landed in a
different agent's worktree, where the looser check passes and nothing
fires. Stage subagents also have no `claim_card` tool, so the dispatcher
holds the claim on their behalf; a brief that asserts a claim nobody took
leaves the subagent unable to record even its own block.

Two settings-level hooks back this up in this repository
(`.claude/settings.json`, `scripts/hooks/`): one denies destructive git
commands run outside `.claude/worktrees/`, the other records the working
directory of every shell call so a lapse is measurable rather than found
by accident. Neither is a substitute for passing isolation correctly;
they exist because passing it correctly keeps failing.

## Conventions

- `dev` is the trunk agents write to. Card work goes on
  `card/<human_id>`, branched from `origin/dev` and pushed there, and
  Merge lands it on `dev`. `master` is the public line and only a cut
  advances it, after the operator accepts the work in Dev Acceptance.
  Never push card work at `master`, `beta` or `stable`; the columns
  carry `trunk_branch` and `protected_branches` and those directives are
  the authority if this file ever disagrees with them again.
- Commits for card work carry the `maize-NN: ` prefix.
- Handoff notes follow the pointer-note protocol: full report as a card
  comment, short note with the required headings plus the comment link.
- No em-dashes and no smart quotes in committed text, card comments, or
  handoff notes.
