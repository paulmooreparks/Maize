# Maize project instructions

## The board is the source of truth

This project's backlog lives on the **maize** workbench in Andoneer
(yak.andoneer.com, via the `andoneer` MCP connector). Chat is a sidecar;
the board records what is being worked, by whom, and what stage it is in.

**Claim before you work.** A card being worked without a claim is dark
work: the operator cannot see it in flight. The claim is taken the moment
output starts, not when the card is about to move, and it is released or
blocked rather than held while idle.

Who takes it depends on where you are. A session working a card directly
calls `claim_card` or `pull_next` itself. A stage subagent cannot: those
agents have no `claim_card` tool, by design, so the dispatching session
holds the claim on their behalf and says so in the brief. Do not tell a
subagent a claim is held unless you have taken it, because a subagent
that owns no claim cannot record its own block either.

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

- **You never push a branch other than your own card branch.** Card work
  goes on `card/<human_id>`, branched from `origin/dev` and pushed to
  `origin/card/<human_id>`. `dev` is the integration line and is reached
  only through Merge, which is the one stage that pushes it. `master` is
  the public line and only a cut advances it, after the operator accepts
  the work in Dev Acceptance. `beta` and `stable` do not exist yet and
  will follow the same rule when they do. If you find yourself about to
  push `dev`, `master`, `beta` or `stable`, you are outside your stage.
  The columns carry `trunk_branch`, `branch_pattern`, `push_policy` and
  `protected_branches`, and those directives are the authority if this
  file ever disagrees with them again.
- Commits for card work carry the `maize-NN: ` prefix.
- Handoff notes follow the pointer-note protocol: the full report goes in
  a card comment, and the move-note stays short and links to it. Columns
  enforce their own exit checklist on that note, so read the column for
  which sections it requires rather than assuming; several want
  `## WHAT SHIPPED` and `## WHAT I CUT`, and a move is refused outright
  when a required section is missing.
- No em-dashes and no smart quotes in committed text, card comments, or
  handoff notes.
