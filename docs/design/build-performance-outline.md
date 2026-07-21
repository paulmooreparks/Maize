# Build Performance: Measurement Spike Outline

Status: solution-outline deliverable for maize-301, the outline stage of epic maize-299,
against the design doc docs/design/build-performance.md (maize-300). This document reports
the measurement spike (design doc section 6, step 0), the confirmed cost ranking, a
committed numeric target, and a proposed (not filed) implementation-card breakdown aimed
from the numbers. No implementation cards are filed by this document; the aiming decision
below is offered for adversarial review before any card is created, per the operator's
explicit instruction on this card.

## 1. Method

Throwaway per-phase wall-clock instrumentation was added to `src/mzcc.c` (wrapping the
`cpp`, `cproc-qbe`, `qbe`, and `mazm` spawn sites, the in-process `normalize_buffer` call,
and `mzld_link`), gated on `MZCC_PHASE_TIMING=1` so default behavior is unchanged. It was
reverted before this commit; it never shipped. `mzld` link cost was cross-checked two ways:
via the same instrumentation (tagged with the object count per link) and by capturing the
`.mzo` intermediates of a real build (a `MZCC_KEEP_SCRATCH=1` env var was added, also
reverted, to skip the scratch-dir cleanup) and re-linking them standalone. The pre-existing
`MAIZE_CACHE_STATS=1` (maize-274) was used to independently confirm cache hit/miss behavior.

Workloads: a trivial TU (`hello.c`, a two-line `puts` program; the real sbase `echo.c`
could not be compiled standalone because its `util.h` unconditionally pulls `regex.h`,
which only resolves inside the shim-overlaid tree that `build-userland` stages, not against
the raw submodule checkout) and the wave-2 default userland set (43 programs: the 10
wave-1 sbase tools, 32 wave-2 sbase tools, and oksh) via `mzcc build-userland`.

Both cache regimes were measured and pinned per run: TRUE COLD sets
`MAIZE_NO_OBJECT_CACHE=1` (the cache is neither read nor written, so every TU recompiles
every time regardless of what ran before it in the same batch); TRUE WARM pre-populates the
on-disk cache with an unrelated prior run of the identical inputs, then measures an
unchanged rebuild with the cache left enabled. A third condition, "scrub-only cold" (the
cache dir is deleted before the run but stays enabled, so it self-populates as the batch
progresses), is reported separately because it is a real and useful number (a first full
build after a fresh clone) but is NOT the isolated per-phase reference the design doc's
protocol calls for, since after the first occurrence of the runtime/shared-helper sources
every later program in the same batch gets warm hits from the run's own writes. This
distinction mattered in practice: an early attempt to use "scrub-only cold" as the cold
reference under-counted true cold cost by roughly 1.6x on this corpus (159 vs 707
cproc-qbe/qbe invocations).

Per-phase attribution was taken at `MAIZE_JOBS=1` (serial, no scheduler overlap between
TUs); end-to-end wall clock was taken separately at the real default `-j` (`max(2,
nproc-2)` = 30 on this 32-logical-core host).

Hosts: Windows (windows-llvm-mingw-release, the required primary) and, as a secondary data
point, Linux under WSL2 (Ubuntu-24.04, GCC 13.3, linux-release preset), built in the same
isolated worktree. All work happened in an isolated git worktree off origin/master; the
operator's root working tree was never touched. Builds ran solo throughout (never two heavy
builds concurrently), per the explicit instruction on this card.

## 2. Cost ranking

### 2a. Windows, multi-TU corpus (43 programs), -j1 clean per-phase reference

TRUE WARM, 132.29s wall clock total:

- cpp spawns: 126.60s (95.7% of wall clock). Of the 707 total cpp calls, 473 are the 11
  RT-C libc sources recompiled once per program (66.9% of all cpp calls); at the measured
  per-call average this is roughly 84.7s, i.e. about 64% of the ENTIRE warm multi-TU build,
  spent re-preprocessing runtime sources whose output never changes, purely to recompute a
  cache key.
- User-TU cpp spawns (the other 234 calls): roughly 41.9s (31.7%).
- `mzld` link: 3.42s (2.6%), 43 links.
- Driver/staging overhead (stage_project tree copy + shim overlay, run once per project,
  plus per-tool bookkeeping): roughly 2.3s (1.7%).
- `cproc-qbe` / `qbe` / `mazm`(body) / normalize: ZERO invocations. 100% object-cache hit
  rate, confirmed both by the instrumentation (no non-cpp/non-mzld spawns logged) and
  independently by `MAIZE_CACHE_STATS` on the single-TU case (see 2c).

TRUE COLD (`MAIZE_NO_OBJECT_CACHE=1`), 255.49s wall clock total:

- cpp: 112.80s (44.2%), 707 calls, avg 159.6ms.
- mazm: 48.48s (19.0%), 836 calls (707 C-pipeline bodies + 129 RT_ASM), avg 58.0ms.
- qbe: 45.00s (17.6%), 707 calls, avg 63.7ms.
- cproc-qbe: 42.24s (16.5%), 707 calls, avg 59.7ms.
- mzld: 2.56s (1.0%), 43 links, avg 59.5ms.
- normalize (in-process): negligible (well under 0.1s total).
- Driver/staging overhead: roughly 4.4s (1.7%).

"Scrub-only cold" (cache enabled, dir scrubbed once, then self-populates across the batch),
157.21s wall clock: cpp 118993ms (707 calls), cproc-qbe 11883ms / qbe 10646ms / mazm
10381ms (all n=159-162, i.e. only the FIRST occurrence of each unique
runtime/shared-helper/user source in the batch actually recompiled), mzld 1917ms (43
links). Reported for completeness (this is the realistic "first build after a fresh
clone" number) but excluded from the ranking above because it under-represents true
per-TU cold cost.

### 2b. Windows, end-to-end wall clock at real default `-j` (30 workers)

- Multi-TU COLD (scrub-only): 88.72s.
- Multi-TU WARM: 65.41s.
- Only a 26% reduction despite a 100%-hit warm object cache: the cache never touches the
  never-cached link or the always-run cpp spawns, so parallelism (not caching) is doing
  most of the visible work in this comparison.
- Single-TU (`hello.c`) WARM at real `-j`, 3 reps: 1.240s / 1.838s / 1.967s, versus 1.168s
  at `-j1`. Parallelism was measured SLOWER than serial for a 12-spawn job: worker-pool
  setup overhead exceeds the overlap benefit below some per-build spawn-count floor. This
  matters for the persistent-driver design (step 4): amortizing spawn overhead across many
  builds only pays off at scale, and a driver invoked for a tiny single-file edit-compile
  loop could be a wash or a regression unless it specifically avoids per-invocation
  worker-pool setup cost for small jobs.

### 2c. Windows, single trivial TU, -j1

WARM: wall clock 1.168s. `MAIZE_CACHE_STATS` confirmed a cache hit on ALL 15 objects (3
RT_ASM + 11 RT_C + 1 user body); only cpp ran, 12 times (11 RT_C + 1 user, avg ~86ms, sum
~1029ms), plus `mzld` (nobj=15, ~20ms). This is a direct, explicit confirmation of the
design doc's warm-TU spawn account (section 2): exactly 1 cpp spawn per warm TU, 11 of
which are the RT-C libc modules, present on every single program.

COLD: wall clock 3.484s (after the first-touch AV tax had already been paid against these
exact binaries in an earlier run; see confounds below); sum of 64 PHASE lines 3380.75ms,
consistent with the wall clock.

### 2d. Linux (WSL2), same workloads, same host machine

Single-TU WARM, -j1: wall clock 0.307s (versus Windows's 1.168s, 3.8x faster). cpp avg
~19.3ms per spawn (11 calls); mzld link ~5.2ms.

Multi-TU (43 programs), -j1:
- TRUE COLD: wall clock 53.55s (vs Windows's 255.49s, 4.8x). cpp 14.48s avg 20.5ms;
  cproc-qbe 6.43s avg 9.1ms; qbe 10.82s avg 15.3ms; mazm 14.56s avg 17.4ms; mzld 0.22s avg
  5.2ms. No AV-attributable outliers at all (max cpp observed: 55.5ms, vs Windows's
  2117ms).
- TRUE WARM: wall clock 21.11s (vs Windows's 132.29s, 6.3x). cpp 14.44s avg 20.4ms (707
  calls, unchanged since cpp always runs); mzld 0.23s avg 5.4ms; cproc-qbe/qbe/mazm(body) =
  0 (100% hit).

Multi-TU WARM at real default `-j` (30 workers): wall clock 3.92s (vs Windows's 65.41s,
16.7x). Parallelism bought roughly 5.4x on Linux (21.1s to 3.9s) versus only about 2.0x on
Windows (132.3s to 65.4s) for the IDENTICAL warm workload: Windows's per-spawn overhead
(CreateProcess plus, per the confound below, an AV scan queue) resists overlap far more
than Linux's fork/exec.

Caveat: this WSL run reads and writes through `/mnt/c` (DrvFs over the Windows NTFS
volume, not native ext4), which is known to carry a per-file-operation tax. The Linux TRUE
WARM breakdown has roughly 6.5s (21.1s wall minus 14.4s cpp minus 0.23s mzld) unaccounted
for, a much larger relative share (about 31%) than Windows's 1.7%; this is very likely
DrvFs overhead on the `stage_project()` tree-copy-plus-shim-overlay step, not a genuine
Linux driver cost. A native ext4 clone would likely show this residual shrink further,
which would widen the Windows-vs-Linux gap on driver overhead specifically (not on the
cpp/link numbers, which are pure spawn-plus-compute cost and not filesystem-path-dependent
in the same way).

### 2e. mzld link cost versus object count (Windows, real corpus runs)

Steady-state (excluding AV-attributable outliers, about 12% of links, 450-560ms each):
nobj=15 to 21 -> 19-27ms; nobj=24, 26 -> 20-23ms; nobj=53 (oksh, the largest program in the
corpus) -> 27ms. Link cost is real but essentially flat across a 3.5x object-count range
(roughly 0.2ms per additional object). This directly bounds the upside of `mzld` member
selection: even eliminating link cost entirely on this corpus caps at 2.6 percentage
points of warm wall clock.

### 2f. Steady-state per-exe spawn cost

The representative figures are the mzcc-native (Win32 `CreateProcessA`, `src/mzcc_proc_win32.c`)
steady-state averages embedded in the phase data above: cpp ~160-180ms, cproc-qbe
~60-75ms, qbe ~64-68ms, mazm ~58-65ms, mzld ~45-80ms (Windows); cpp ~20ms, cproc-qbe ~9ms,
qbe ~15ms, mazm ~17ms, mzld ~5ms (Linux). A separate no-op measurement via a Git Bash
loop (`clang --version`, `qbe -h`, etc., 20 reps each) gave notably higher numbers (clang
373ms avg, cproc-qbe 151ms, qbe 145ms, mazm 224ms, mzld 119ms avg): this is the added tax of
shelling out through Git Bash's fork emulation on top of the same underlying exe, not a
property of the tools themselves, and is excluded from the ranking above as unrepresentative
of mzcc's own (native, non-bash) spawn path. A PowerShell `Start-Process` cross-check was
even less representative (roughly 1050-1085ms avg, dominated by .NET process-launch
overhead) and is reported only to rule it out as a measurement method.

## 3. Confirm or refute the design doc's section-2 account

The design doc's cost model (section 2) is directionally right that COLD is dominated by
the compile chain, not the link: measured, the four-stage chain is 97.3% of cold wall
clock, link 1.0%. CONFIRMED.

For WARM, the design doc's numbered list (section 2, "the residual per program is: 1. Link
... 2. Per-TU cpp spawn ... ") reads `Link` as the headline item ("its own tier, not a
sub-item of runtime recompile"), ranked ahead of the surviving cpp spawn. Measured, this is
REFUTED on this corpus and host: link is 2.6% of warm wall clock; the surviving cpp spawns
are 95.7%, and the RT-C portion alone (recompiling 11 fixed libc sources per program purely
to regenerate a cache key) is roughly 64% on its own, about 37x the link cost. The doc's
qualitative claims about link (never cached, scales with runtime size) are correct as
description, but the runtime size in question (14 to 53 objects measured) is small enough
that the absolute cost stays small; the doc's implicit priority ordering does not match the
measured magnitude. This is the single most important corrective finding of this spike, and
it changes which lever to attack first (section 5).

Whole-corpus test scope (design doc item 3) was not independently re-measured in this
spike: `scripts/run-ctest.sh` is a 3400-plus-line harness that rebuilds the whole C corpus
plus quesOS plus doom fixtures on every invocation, corroborating the doc's qualitative
claim, but running it end-to-end for a real number was out of this spike's time and
one-build-at-a-time budget. This is flagged, not silently skipped: the "well over an hour"
Test-stage baseline the card's own description cites almost certainly dwarfs every build
number in this report (the full 43-program corpus builds and links in under 5 minutes even
truly cold), so test SCOPE, not build SPEED, is very likely the dominant lever on the
actual operator-facing pain point, even though this spike's own instrumentation does not
speak to it directly. See section 5.

Userland staging I/O (design doc item 4) is confirmed as real (`stage_project()` does a
fresh `copy_tree` plus a patch series plus a shim-header overlay into every subdirectory,
once per project per invocation) but measured small on today's corpus: roughly 1.7% of
Windows warm wall clock. It will matter more as the userland set keeps growing.

## 4. Confounds

- Windows Defender: `AntivirusEnabled=True`, `RealTimeProtectionEnabled=True`,
  `IoavProtectionEnabled=True`; this session had no admin rights to add a build-tree
  exclusion or enumerate existing ones. Qualitative evidence of a real tax: the very first
  touch of a freshly-scrubbed corpus showed isolated phase spikes up to 2117ms (cpp),
  1932ms (cproc-qbe), 1471ms (qbe), 1057ms (mazm), and 563ms (mzld) that did not recur on
  later runs against the same binaries and paths, consistent with a per-first-touch scan
  tax rather than a steady per-invocation one. About 12% of the 43 `mzld` links in a given
  corpus run still showed a 450-560ms spike, suggesting the tax also recurs opportunistically
  (e.g. after Defender's definition/behavior cache ages out), not purely once-per-binary.
  Recommendation: whoever picks up the first implementation card should get an admin-level
  before/after comparison with the build tree excluded; this spike could not obtain one.
- OS page cache: not independently isolated from the object cache in this spike (both
  regimes were run back-to-back on the same warm OS page cache). Not expected to be a large
  factor given how small the source/object files are, but not proven here.
- Thermal/frequency drift: the three Windows full-corpus -j1 runs (scrub-cold 157.2s,
  true-warm 132.3s, true-cold 255.5s) ran back-to-back over roughly 45 minutes of
  continuous building with no anomalous slowdown relative to what their own steady-state
  per-phase averages predict. No material drift observed at the grain this spike measured.
- Fork-contention ceiling (the `dofork ... Resource temporarily unavailable` failure from
  the maize-292 test, hit when two heavy git-bash-driven builds ran concurrently): this
  spike deliberately ran solo throughout, per its own instruction, so the failure was not
  reproduced. A bounded, explicitly non-heavy test (up to 300 concurrent Git-Bash-launched
  subshells, each itself forking a nested subshell) completed cleanly with zero resource
  errors, which indicates the limit is tied to the resource footprint of a HEAVY build
  (many nested forks plus a larger memory footprint per build), not to raw concurrent
  process count. The precise numeric ceiling for concurrent heavy builds was not re-derived
  here; a dedicated ramp test (one heavy build, then two) would be needed to pin it exactly,
  and was skipped here to avoid corrupting this spike's own timing data or destabilizing the
  shared machine.
- DrvFs (WSL over `/mnt/c`): see section 2d. Inflates the Linux driver/staging-overhead
  share; does not affect the cpp/link numbers in a comparable way.

## 5. Committed numeric target

The measured floor, once RT_C recompute is fully eliminated (no other change):

- Windows, warm, trivial TU: 1 cpp spawn (~80-180ms measured) + 1 `mzld` link (~20ms) =
  roughly 100-200ms. This does NOT reach the design doc's suggested "low tens of ms"
  without also attacking spawn cost itself (a single Windows `CreateProcess` plus vendored
  clang preprocessor invocation floors any design that still shells out to a real cpp).
  Proposed near-term target: **under 200ms** for a warm trivial-TU Windows build once the
  RT-C spawns are eliminated (a ~6-12x improvement over today's measured 1.168s). A
  longer-term target of low tens of ms is achievable only after in-process preprocessing
  (design doc pillar 2's last stage) removes the cpp spawn itself; that is correctly
  sequenced last (step 3) and should not gate this card.
- Linux, warm, trivial TU: measured floor already close to target today (~25ms: 1 cpp
  ~19ms + link ~5ms). Proposed target: **under 50ms**, achievable now once RT_C recompute
  is eliminated on the Linux leg too (same lever, same code path).
- Multi-TU (43-program corpus) full warm rebuild: measured today at 65.4s (Windows,
  real-j) / 3.9s (Linux, real-j). Eliminating the ~64% RT_C-cpp share projects to roughly
  24-30s Windows / under 2s Linux for the SAME corpus, all builds combined, once the first
  card lands. Proposed target: **under 30s Windows / under 5s Linux** for a full warm
  wave-2 userland rebuild.
- Per-card Test stage: NOT reachable by any build-speed lever measured in this spike (the
  full corpus already builds in under 5 minutes even truly cold; the Test stage's
  "well over an hour" cost is not explained by build time at all on these numbers).
  Proposed target: **single-digit minutes**, committed as the doc already proposes, but
  explicitly gated on a test-SCOPING card (rebuild and check only what a given card
  touched, plus a fast regression subset), not on any of the compile-pipeline levers this
  spike measured. This is the clearest actionable conclusion of the spike: build-speed
  work and test-speed work are different problems with different owners, and the
  measured numbers do not support treating the compile-pipeline levers as a Test-stage
  fix.

## 6. Proposed implementation-card breakdown (NOT filed; for adversarial review)

Ordered by measured payoff first, then risk. Two candidate shapes are given for the first
card because the measured 64% payoff is available at two different risk levels, and this is
exactly the kind of aiming choice the operator asked to review before cards are locked.

1. **Eliminate RT_C cpp-recompute cost.** Measured justification: roughly 64% of the
   Windows warm multi-TU wall clock (84.7 of 132.3s measured on the 43-program corpus),
   scaling with corpus size (every future userland wave repays this tax on every program).
   Two shapes, same target:
   - 1a. Narrow: cache (or otherwise skip) cpp output specifically for the 11 fixed RT_C
     sources, keyed on the toolchain fingerprint already computed in `mzcc_cache.c`
     (`mzcc_cache_configure`), so a user build never re-invokes cpp for the runtime at all.
     Risk: LOW. Pure addition to the existing cache layer; no new archive container, no
     `mzld` change, no format/ABI decision.
   - 1b. Full prebuilt runtime archive (pillar 1, maize-76, the operator's stated
     priority). Same ~64% cpp-avoidance payoff, PLUS the (small, measured at most 2.6
     percentage points) link-side win from member selection, PLUS a cold-regime win (RT
     sources never compile at all, not just never re-cpp'd). Risk: MEDIUM. Requires an
     archive container decision (explicitly an internal, replaceable choice per design doc
     section 5, so not a forward-compat risk) and teaches `mzld` member selection.
   Recommendation for review: 1a is the cheaper, lower-risk way to capture the dominant
   measured win immediately; 1b is the operator's already-named priority and captures
   slightly more (member selection, cold-regime) at real but bounded extra risk. Both
   converge on the same target number in section 5.

2. **Per-card test scoping** (pillar 3). Measured justification: not independently timed
   in this spike (see section 3), but the existing "well over an hour" Test-stage baseline
   is 10-50x every build number this spike measured, and no compile-pipeline lever
   measured here touches it. Risk: MEDIUM-HIGH. Needs a real "what did this card touch,
   what does it affect" dependency graph and a policy decision on the regression subset,
   more design work than a caching layer; should get its own spike/spec pass before
   implementation, similar in spirit to this one.

3. **qbe batching** (cheap lever, take opportunistically). Measured justification: zero
   effect in the WARM regime measured here (0 qbe invocations, fully cached), but qbe is
   17.6% of COLD wall clock (707 invocations); the design doc's native multi-file mode
   could collapse per-program qbe invocations toward 1, a meaningful cold-build and
   cache-miss-burst win (e.g. after a shared-header change invalidates many entries at
   once). Risk: LOW (qbe already resets its per-function arena; no library-ification
   needed).

4. **mzld member selection / dead-code elimination.** Measured justification: SMALL today
   (2.6% of warm wall clock; link cost is nearly flat from nobj=15 to nobj=53, about
   0.2ms/object). Real and structurally exactly as the design doc describes (unconditional
   placement, confirmed by reading `src/mzld.cpp:295-309` and `:449-482`), and the one cost
   that never benefits from any cache, but not urgent on today's corpus size. Revisit after
   card 1 lands and the corpus keeps growing. Risk: MEDIUM (a real liveness/reachability
   pass over relocations; self-contained, no ABI change per design doc section 5).

5. **Deferred, matching the design doc's own step 3/4 ordering** (in-process mazm/mzld,
   then qbe, then cproc-qbe plus integrated preprocessing, then the persistent driver): no
   data in this spike argues for reordering these ahead of 1-4. One new caution from this
   spike for whoever picks up the persistent-driver step: at low per-build spawn counts
   (measured on the single-TU case), overlapping work via a worker pool was SLOWER than
   serial (1.24-1.97s at real `-j` vs 1.168s at `-j1`), so a resident driver's benefit
   (amortizing spawn overhead across many builds) is real only at scale; a driver invoked
   for a tiny single-file edit-compile loop needs to avoid paying worker-pool setup cost on
   small jobs or it could regress today's already-fast single-shot path.

## 7. Interface / format decisions (brief)

Nothing in this spike's numbers argues against the design doc's section 5 ruling: the
`.mzo` object format, its symbol/relocation tables, and the calling convention stay the
forward-compatibility surface (dynamic-linking-ready, JIT-ready); the archive container for
card 1b is confirmed, by direct code reading (`src/mzld.cpp`, `src/mzcc_cache.c`), to be an
internal, replaceable implementation choice that leaks into no contract. The scheduler
(`src/mzcc_sched.c`, a shared-next-index thread pool) is a reasonable starting shape for a
future persistent driver: it already separates "what work exists" from "how it's dispatched",
so pillar 2's library-ified stages could plug into the same pull-based worker loop rather
than needing a new dispatch mechanism.

## 8. What was not measured, and why

- Whole-corpus `run-ctest.sh` timing: out of this spike's time and one-build-at-a-time
  budget (see section 3); recommend it be owned by the test-scoping card's own spike.
- Precise concurrent-heavy-build fork-contention ceiling: deliberately not reproduced (see
  section 4); this spike ran solo throughout as instructed.
- OS page-cache cold vs warm, isolated from the object cache: not separately controlled
  (see section 4).
- A from-scratch (non-WSL, native ext4) Linux host: WSL2-over-DrvFs was used as the
  available secondary host; the driver/staging-overhead share reported for Linux is
  probably inflated by DrvFs (see section 2d), though the cpp/link numbers, which drive the
  ranking, are not expected to be materially affected.
