# Build Performance: Real Toolchain Architecture

Status: design, revised after adversarial review (maize-300 comment #3115). Epic:
maize-299. This document is the architecture of record for the build-performance
round-2 effort. It defines the target the solution-outline card and the
implementation cards build against. Nothing here is committed to code yet; the
sequencing is deliberately gated on measurement, and the cost model below
distinguishes warm-cache from cold-cache regimes because the two are ranked
differently and the motivating slowness lives in the warm regime.

## 1. Problem

A trivial C utility (for example sbase `echo`) takes seconds to compile, and a
per-card Test stage runs well over an hour. The cost grows with the size of the
userland, so every wave of new tools makes the next build slower. This is a
structural problem, not a tuning problem.

Two prior cards addressed adjacent costs and are not the fix here:

- maize-291 repointed `build-world` from `sh`-forwarder scripts to the compiled
  `mzcc` driver (a roughly 46x wall-clock win). That removed orchestration
  overhead, not per-translation-unit cost.
- maize-274 added a content-addressed object cache (persistent, per-user, on disk
  at `%LOCALAPPDATA%/maize/objects` or `$XDG_CACHE_HOME/maize/objects`;
  `src/mzcc_cache.c:114-150`) plus a parallel TU scheduler. This is load-bearing
  for the analysis below: on a warm cache an unchanged TU is not recompiled, so the
  steady-state cost is not what a cold build would suggest.

The remaining cost splits by regime, and the motivating slowness is the warm one.

## 2. Cost model (warm vs cold)

The current guest-C pipeline, per TU, is (with what is a process spawn marked):

    cpp -E [spawn]  ->  cproc-qbe [spawn]  ->  normalize [in-process,
    normalize_buffer src/mzcc.c:534]  ->  qbe -t maize [spawn]  ->  mazm -c [spawn]

and then a per-program link (once per program, not per TU):

    mzld [spawn]  (crt0 + syscall + string + ctype + stdio + stdlib + program objects)

Spawn accounting, corrected against `src/mzcc.c` (`compile_tu_ex`):

- Cold TU: 4 spawns (cpp `:737`, cproc-qbe `:779`, qbe `:804`, mazm `:634`).
  `normalize` is an in-memory transform, not a spawn.
- Warm TU (object-cache hit): 1 spawn. Only cpp runs, because the cpp output is the
  cache key and the lookup at `:765` happens after the cpp spawn at `:737`;
  cproc-qbe/qbe/mazm are skipped on a hit.
- Per program: 1 additional `mzld` spawn, and the runtime contributes 11 RT-C cpp
  spawns per program even on a warm build, because each RT source is cpp'd to
  regenerate its cache key before the object lookup.

Cost by regime:

Cold cache (first build, or a changed TU): dominated by the 4-spawn compile chain
per TU, times TUs, plus the per-program link. Runtime sources compile here too.

Warm cache (the steady state, and the regime of the hour-plus Test stage, which
re-runs `run-ctest.sh` against a mostly-unchanged corpus): the object cache serves
cproc-qbe/qbe/mazm output, so the residual per program is:

1. Link. `mzld` relinks the full object set from scratch on every program, warm or
   cold, and places every section of every input unconditionally with no
   dead-code elimination or archive-member selection (`src/mzld.cpp:295-305`,
   `:449-481`). This is the one cost that is never cached and scales with runtime
   size on every single program. It is its own tier, not a sub-item of "runtime
   recompile."
2. Per-TU cpp spawn. The one spawn that survives a warm build, once per user TU and
   11 times per program for the RT-C sources (to regenerate cache keys).
3. Whole-corpus test scope. `run-ctest.sh` rebuilds the entire C corpus plus quesOS
   plus doom fixtures every run, with no per-card scoping, so test cost scales with
   total project size rather than with what changed.
4. Userland staging I/O. `build-userland` copies a fresh submodule tree, applies the
   patch series, and fans shim headers into every subdirectory on every invocation
   (`src/mzcc_userland.c:143-208`); the stage cache was dropped (DI 9621). Real I/O
   the earlier draft omitted.

Consequence for sequencing: the prebuilt-runtime archive (section 4, pillar 1) is
still the operator's named priority and a strong early card, but on a warm cache its
benefit is not "stop recompiling the runtime" (already cached); it is (a) removing
the 11 RT-C cpp spawns per program, and (b) enabling member selection so the relink
is smaller. Those are real, but whether they outrank per-program relink cost,
per-user-TU cpp, and whole-corpus test scope is a question for measurement, not for
this section. See section 6.

The numbers above are a structural account, not a timing. The first deliverable
after this document is a measurement spike (section 6) that produces the real
per-phase split in both regimes. We choose what to attack from the numbers.

## 3. How a real toolchain achieves speed

The reference model, from clang/LLVM, musl, Go, and Make/Ninja/Bazel:

1. The compiler is one process. Preprocess, parse, optimize, emit, in a single
   address space. No process per phase.
2. The C runtime is prebuilt. `libc.a` and `crt0.o` are built once at toolchain
   install and linked into every program; nobody recompiles libc for hello-world.
3. Separate compilation plus an incremental graph. Each source compiles to an
   object once; a build graph rebuilds only what changed and parallelizes; objects
   are cached; preprocessed output can be cached too (ccache direct mode).
4. The linker is a fast single pass over prebuilt objects and archives, pulling
   only the members it needs.
5. High-throughput builds keep the compiler resident (persistent workers, or a
   batching driver) to amortize even one process start.

Maize violates 1 and 5 outright, implements 2 not at all (no prebuilt runtime),
implements 3 as an object cache without a real dependency graph, cpp-output cache,
or separable runtime, and violates 4 (the linker selects no members and strips
nothing).

## 4. Target architecture for Maize

Three pillars, plus two cheap levers that fall out of the cost model and do not
require the pillars.

### Pillar 1: prebuilt static runtime archive

Build crt0 and the freestanding libc once, at `mzcc --build`/install time, into a
linkable archive; every program links against it and the linker pulls only the
members it references. The runtime is never rebuilt as part of building a user
program. This is maize-76, promoted to load-bearing, and the operator-named
priority. Its warm-cache benefit is removing the 11 RT-C cpp spawns per program and
enabling member selection (a smaller relink); its cold-cache benefit additionally
includes not recompiling the runtime sources. It requires an archive container for
`.mzo` objects (an `ar`-style member index, or a single pre-linked runtime object)
and `mzld` teaching to select members. The archive container is an internal,
replaceable implementation choice (section 5); the forward-compatibility obligation
lives on the object format and ABI, not on the container.

### Pillar 2: in-process compiler

Collapse the spawn chain into one address space: link qbe, mazm, and mzld into
`mzcc` as libraries, and extend preprocessing in-process, so a TU is compiled
front-to-back by function calls. End state: a persistent driver where one resident
`mzcc` process builds the whole userland.

Staged by risk, because the pieces are unequal (verified against source):

- mazm and mzld are our own code; exposing them as libraries carries no upstream
  divergence. They are also the cheapest stages, so doing them first proves the
  in-process harness at low risk but may move little wall-clock unless spawn count
  (not per-stage work) dominates. The measurement resolves that tension; do not
  assume "our code first" is also "biggest win first."
- qbe is tractable-but-real: it already resets its per-function arena
  (`freeall`, `toolchain/qbe/util.c`), but carries process-lifetime globals (the
  string-interning table `itbl`, `typ`/`ntyp`, `curi`, the output `FILE*`) and
  `exit()`s on error. In-process reuse needs those globals reset per TU and an
  error-interception shim. Before that, note the batching lever below, which may
  make library-ifying qbe unnecessary.
- cproc-qbe is the hard piece, harder than the first draft implied: it has no arena
  at all and leaks every allocation, leaning entirely on process teardown, and it
  exits on any diagnostic. Its preprocessor (`toolchain/cproc/pp.c`) does macro
  expansion but errors on `#include` and the `#if` family (`pp.c:319-335`), which is
  why we shell out to a full external cpp. "Integrated preprocessing" therefore
  means extending `pp.c` to handle `#include`/`#if`, or vendoring a preprocessor
  library, not switching on a preprocessor that already exists. This stage is gated
  on a de-risking sub-spike and comes last.

### Pillar 3: incremental build graph and test scoping

A real dependency graph over pillars 1 and 2: relink a program only when its objects
or the runtime archive changed, keep the maize-274 object cache, parallelize, and
ideally run inside the single resident process from pillar 2. Test scoping lives
here and may be the largest single lever on the hour-plus Test stage: a per-card
test should build and check what the card touched plus a fast regression subset, and
reserve the whole-corpus sweep for the Merge/CI gate.

### Cheap levers (no pillar required; evaluate first)

- qbe batching. qbe's `main()` already processes many functions and many input
  files per invocation (arena reset per function), so feeding a whole program's
  runtime-plus-user IL through one qbe process cuts qbe spawns from roughly 14-16
  per program to 1, with zero library-ification and no state-reset risk. This is
  qbe's native mode. It is not safe for cproc-qbe, which concatenates multiple file
  arguments into a single translation unit. A targeted cold-build win.
- Direct-mode cpp caching. On a warm build cpp is the only surviving per-TU spawn
  (its output is the object-cache key). Caching preprocessed bytes keyed on the
  source plus its resolved include set (ccache direct mode) removes that last warm
  spawn, and complements the prebuilt runtime by also removing the 11 RT-C cpp
  spawns per program without an archive.

## 5. Dynamic linking: deferred, forward-compatible

Not part of this epic. Rationale:

- It does not fix the problem. The fastest toolchains produce static binaries
  (musl-static, Go); their links are milliseconds and their programs start
  instantly. The compile-speed problem is spawn overhead, per-program relink, and
  test scope, all solved by static means.
- It costs runtime speed on the wrong layer. Dynamic linking adds load-time
  relocation and a PLT/GOT indirection on every cross-module call. On a VM whose
  bottleneck is interpretation (the reason the JIT, maize-181, is roadmapped),
  taxing every libc call with an indirection is backwards; it belongs after the JIT
  so the indirection is compiled away rather than paid on the interpreter hot path.
- Its real payoff is memory-sharing across processes and a shared-object ecosystem,
  neither of which we have yet.
- It is a whole quesOS project: a dynamic loader, format extensions (dynamic symbol
  and relocation tables, a `.so` form), and a PIC-or-load-time-reloc ABI.

Forward-compatibility, correctly located: a `.so` is produced *from* objects; you do
not extend a `.a`-style archive container into one. So the artifact that must stay
dynamic-ready is the **`.mzo` object format plus its symbol and relocation tables
plus the calling convention**, which both static archives and future shared objects
consume. The archive *container* chosen in pillar 1 is an internal, replaceable
implementation detail that leaks into no external contract, and by the project DIRT
ruling that makes it not debt. Keep the object format and ABI dynamic-ready; do not
over-constrain the archive container. Dynamic linking gets its own future epic,
sequenced after the JIT.

## 6. Sequencing (measurement-gated)

Operator-ratified rule: confirm the biggest cost by measurement, attack it first,
re-measure, fix what remains in the least-hackish, non-corner-painting way, and stop
when speed is reasonable. Do not guess.

Step 0, measurement spike (owned by the solution-outline card). Instrument one
trivial TU (`echo`-class) and one multi-TU build (wave-2 userland), and produce a
per-phase wall-clock breakdown, in BOTH cache regimes, on Windows and Linux:

- Separate warm-object-cache from cold-object-cache runs, and pin cache state per
  run (`MAIZE_NO_OBJECT_CACHE` or a scrubbed cache dir) so hits/misses do not
  confound per-phase timing.
- Take the per-phase split at `MAIZE_JOBS=1`; the scheduler overlaps stages across
  TUs at real `-j`, so only `-j1` gives clean per-stage numbers. Take end-to-end
  wall-clock separately at real `-j`.
- Control the confounds that dominate spawn cost: Windows Defender / AV real-time
  scanning of each spawned exe (the largest and most variable Windows spawn tax;
  measure with the build tree excluded and not), OS page-cache warm/cold (distinct
  from the object cache), and thermal/frequency drift over a long run.
- Attribute the runtime portion of each link separately from the user-code portion,
  and time `mzld` on its own, since link is the never-cached tier.

Deliverable: a table that ranks the costs in each regime and confirms or refutes the
section-2 account. Also required before any implementation card starts: a committed
numeric definition of "reasonable" (see the guardrail in section 7).

Step 1, aim the first implementation card from the numbers. The strong candidates,
any of which the spike may rank first:

- Prebuilt runtime archive (pillar 1): the operator priority; warm win is removing
  11 cpp spawns/program plus member selection.
- Per-card test scoping (pillar 3): likely the largest lever on the hour-plus Test
  stage specifically.
- Direct-mode cpp caching and qbe batching (cheap levers): small, low-risk, may be
  worth taking early regardless.

Step 2, re-measure after each landed change and let the new dominant cost pick the
next card.

Step 3, in-process pipeline (pillar 2), our code first (mazm/mzld), then qbe (or
qbe batching if the spike shows batching captures most of qbe's spawn cost without
library-ification), then cproc-qbe plus integrated preprocessing, gated on the
de-risking sub-spike.

Step 4, persistent build driver and incremental graph (pillar 3), if the numbers
after step 3 still justify it.

Stop when build speed meets the committed target; we are not obligated to complete
every step.

Alternatives considered and rejected: a persistent build daemon that keeps the
existing tool binaries resident over pipes does not dodge the hard problem, because
qbe and cproc-qbe `exit()` and leak per run, so a resident worker needs the same
per-TU state reset that library-ification does; a `fork`-based pre-fork worker pool
would sidestep reset via process isolation but `fork` is unavailable on Windows, the
platform where spawn cost is worst. Library-ification (plus batching) is therefore
preferred over a daemon.

## 7. Guardrails (non-negotiable)

- Measure, do not guess. Every sequencing choice is backed by a timing run in the
  correct cache regime, and every landed change is verified by real build and test
  runs.
- Committed target. The solution-outline card must fix a numeric definition of
  "reasonable build speed" (for example, a trivial TU in low tens of milliseconds
  warm, and a per-card test in single-digit minutes) before any implementation card
  starts, so we neither under-build nor gold-plate.
- Determinism preserved. The stable `__FILE__` / repo-relative identity work
  (maize-290; `src/mzcc.c` line-marker path) must survive the in-process rewrite;
  byte-for-byte fixture stability is a gate.
- Build-dependency minimalism. No new host-tool dependency on any build path without
  surfacing it as a decision. Integrating preprocessing must not add a fresh-clone
  install requirement; a vendored library is preferred to a system `cpp`
  requirement.
- Parity gates. Behavioral parity of built programs at every step (hello.mzb /
  baseline fixtures, the ctest corpus). Where byte-identity is claimed it must hold;
  where it is structurally impossible (the whole runtime object set relinks), the
  behavioral bar is the contract and the byte claim is dropped honestly (maize-292
  AC 9688 precedent).
- Forward-compatibility on the right layer. The `.mzo` object format, its symbol and
  relocation tables, and the calling convention stay dynamic-linking-ready and
  JIT-ready. The archive container is explicitly allowed to be a replaceable
  internal choice.
- Adversarial review. Each card's spec and diff get reviewed hard enough to bounce
  bad ideas, with the good ones promoted on their merits. This document was itself
  revised after such a review.

## 8. Risks

- Vendored compiler state. cproc-qbe has no arena and exits on any diagnostic;
  qbe has process-lifetime interning/type globals and exits on error. In-process
  reuse risks state leakage and build-killing error paths. This is the primary
  technical risk and is why the cproc-qbe stage is last and gated on a sub-spike;
  the qbe-batching lever may let us defer or avoid library-ifying qbe.
- Preprocessor integration. Extending `pp.c` for `#include`/`#if`, or vendoring a
  preprocessor, must not regress macro/include behavior the guest sources depend on,
  nor the maize-290 line-marker determinism.
- Windows process model. Spawn cost is platform-skewed (AV scanning included), so
  wins read larger on Windows than Linux; measurements report both.
- Measurement mis-attribution. The warm/cold and `-j1`/`-jN` and AV confounds
  (section 6) are themselves a risk: a spike that fails to control them would aim
  the first card wrong. Controlling them is part of the spike's contract.

(The archive-container format is deliberately not listed as a risk: per section 5 it
is an internal, replaceable choice, not a contract.)

## 9. Deliverables

1. This design document (maize-300).
2. Solution-outline card: the measurement spike (section 6), the confirmed cost
   ranking in both regimes, the committed numeric target, the object-format/ABI
   forward-compat decisions, the archive and in-process interface shapes, and the
   concrete implementation-card breakdown.
3. Implementation cards, aimed from the measurement, each driven through the full
   pipeline with adversarial review and real test verification.
