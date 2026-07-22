# JIT: Tier-Up of Hot Blocks from the Interpreter

Status: design draft, awaiting adversarial review. Umbrella card: maize-181.
This document is the architecture of record for the JIT campaign. It defines
what the compiled tier must beat, the block model, the correctness contracts
it inherits, and a measure-gated phase sequence in the pattern of the
build-performance round (docs/design/build-performance.md). Nothing here is
committed to code yet.

## 1. What the JIT must beat, in numbers

The interpreter is already tuned, and the campaign's premise rests on knowing
exactly where its remaining time goes. Two measurement rounds pin this down.

The maize-307/308 predecode spike proved that dispatch is not the cost. A full
record-and-replay decoded tier with a 99.99 percent replay rate measured
roughly zero percent on DOOM and only six percent on a synthetic
dispatch-bound loop, because the computed-goto plus PGO fetch path is already
near its floor. Interpreter time lives in handler bodies.

The maize-317 callgrind attribution (hosted CPU-bound workload, 52.1B host
instructions for 186M guest instructions, about 280 host instructions per
guest instruction) breaks those bodies down:

- `tick()` itself, 31.6 percent: operand decode, subregister mask selection,
  and the branchy per-opcode plumbing that runs before any semantic work.
- `memory_module::read` / `read_into` and their header clones, about 19
  percent: block-cache probe plus bounds handling on every guest access.
- The `copy_*` register-plumbing helpers, about 16 percent: `reg_value` proxy
  reads and masked subregister writes.
- Sv48 translation residue, 9.4 percent after the maize-317 inline fast-page
  cache (down from 21.2 percent; the remainder is the per-access key check).
- The ALU and lazy-flags machinery, about 8 percent.

Compiled code attacks all five at once. A translated block needs no decode
(it happened at compile time), keeps hot guest registers in host registers
(no proxy, no mask dance), inlines the memory fast path (cache-hit load or
store as a few host instructions), inlines the fast-page translation check,
and can use host flags for compare-and-branch shapes. The honest expectation
from the attribution is a 4x to 8x instruction-rate multiple on compute-bound
guest code, which is the difference between a usable and an unusable
Linux-door build machine (a GCC or LLVM self-host compile at 70 MIPS is not
tolerable; at 400+ MIPS it begins to be).

The motivation is general compute, not DOOM. DOOM is already saturated at its
35 Hz tic rate on the hosted stack (M69 F35 as of 2026-07-22).

## 2. Architecture: two tiers, template codegen, steal the proven model

Tier 0 is the existing interpreter, unchanged in role: cold code, complex
opcodes, and every architectural edge case run there forever. Tier 1 is a
baseline template JIT over basic blocks, in the QEMU-TCG lineage rather than
the tracing lineage: per-opcode emit functions produce host code that mirrors
the interpreter handler's semantics, blocks chain directly to compiled
successors, and anything the emitter does not cover calls out to the
interpreter's own helper for that opcode. There is no optimizing tier, no IR,
no trace formation, and no speculation in this round; those are follow-on
tiers if the measured ceiling of the template tier proves too low.

A block is discovered by the interpreter: a per-entry execution counter in
the block table rides the existing dispatch, and a block that crosses the
hotness threshold (initial value 50, tuned by measurement) is compiled once
and patched into the dispatch path. Cold code is never compiled, which bounds
both compile-time overhead and code-cache growth.

## 3. Block identity, paging, and the four correctness contracts

### 3.1 Blocks are keyed by physical address

A block's key is the physical address of its entry, plus the privilege bit.
Physical keying is what makes the design survive quesOS reality: SATP rewrites
on every context switch, fork sharing parent code pages, and the same kernel
text executing from every process's address space all hit the same compiled
blocks with no flush. This is the QEMU choice and it is right here for the
same reasons. Translation happens once at block entry (the maize-317
fast-page path already makes that cheap); a block never spans a 4 KiB page
boundary, so one entry translation covers every fetch inside it. A block also
ends at any control transfer, at SYS, at any privileged instruction, and at a
size cap.

### 3.2 Self-modifying code invalidates by page

Any write into a page that holds compiled blocks invalidates that page's
blocks. The write sources are enumerable and few: guest stores all funnel
through `memory_module::write_bytes` and its width-specialized siblings, and
host-side writers (the $F4/$F5 bulk accelerators, hostfs reads into guest
memory, the loader, device DMA into the framebuffer region) go through the
memory module's host-facing entry points. A per-page generation bitmap makes
the check one test on the write path for pages with no compiled code, which
is almost all of them. The committed contract fixture is
`asm/test_selfmod.mzb` (both runners); it was landed for exactly this moment
during the maize-307 spike.

### 3.3 Interrupts and the timer stay boundary-precise

The interpreter checks interrupt delivery at instruction boundaries; compiled
code checks at block boundaries and at backward branches (the loop backedge is
the case that matters). The instruction-count timer decrements against a
budget in the CPU context, so a 250,000-instruction slice (maize-319) spans
many blocks and the per-block cost is one decrement and branch. This coarsens
delivery granularity from one instruction to one block, which the platform
already tolerates: nothing in quesOS or the device model depends on
sub-block interrupt latency, and the block-size cap bounds the worst case.

### 3.4 Faults need the precise guest PC

A page fault or alignment fault raised mid-block must report the faulting
instruction's guest PC, not the block entry. Each block carries a small side
table mapping fault-capable host sites (memory accesses) to guest PCs; the
fault path walks it. This is the standard TCG restore mechanism, scoped down
because only memory accesses and explicit trap opcodes can fault; ALU work
cannot.

## 4. What compiled code looks like inside

Register allocation is static, not per-block: the most-frequent architectural
registers by measured static and dynamic frequency get pinned host registers
for the duration of a block's execution (x86-64 has enough spare registers
after reserving the context pointer, the memory-base pointers, and scratch),
and the rest live in the CPU context and are loaded on use. Block entry and
exit spill and fill the pinned set, which is cheap relative to block length
and keeps the interpreter's state layout authoritative between blocks. The
`reg_value` proxy and its subregister masking exist only in the interpreter;
compiled code uses plain loads, stores, and partial-register operations.

Flags stay lazy across the same contract the interpreter uses
(`materialize_flags`): the emitter records the last flag-producing operation
and materializes only when a consumer reads flags, and the common
compare-then-conditional-branch pair compiles to the host's native compare
and jump with no materialization at all.

Guest memory access inlines the two-probe fast path: the fast-page
translation check (Sv48) or nothing (Bare), then the direct-mapped L1 block
cache hit path, then the host load or store. Every miss or edge (cache miss,
page-crossing access, device window) calls out to the existing memory-module
routine, so the slow paths remain single-sourced in the interpreter code.

SYS, IN/OUT, IRET, HALT, TLB and CR writes, and anything rare or intricate
end the block and run interpreted. Coverage of the emitter grows by measured
frequency, never by completeness for its own sake.

## 5. Backend, platforms, and W^X

The emitter is a hand-rolled x86-64 template backend with no external
dependencies (the dependency-minimalism ruling applies; DynASM-style
generation is the model but the emitter is plain C++ writing bytes). The code
cache is a fixed-budget arena with whole-cache flush as the eviction policy
(proven adequate in TCG for years; per-block eviction is complexity with no
measured need). Windows uses VirtualAlloc plus VirtualProtect and
FlushInstructionCache; POSIX uses mmap plus mprotect. Pages are never
writable and executable simultaneously (write, then flip). An AArch64 backend
is a later phase with its own card; nothing in the block model is
x86-specific.

Two observability seams need explicit handling rather than silent breakage.
The maize-261 profiler samples the guest PC from the interpreter dispatch
loop; under the JIT it degrades to block-entry granularity, and `--profile`
documents that (or forces tier-0, operator's choice at implementation time).
The `--show-perf` instruction count stays exact because the block's
instruction count is known at compile time and added at block entry.

## 6. Phases, each gated on a measurement

- J0, block-shape spike (no codegen). Ride the interpreter with block
  discovery and counters; report block-length distribution, reuse rates, and
  the fraction of dynamic instructions inside hot blocks for doom_bench, the
  hosted oksh loop, and a cc-self-compile workload. This validates the block
  model and produces the coverage-ordering data for the emitter. Gate for
  proceeding: hot-block dynamic coverage above 90 percent on compute-bound
  workloads (if it is lower, the block model is wrong and the design returns
  here).
- J1, minimal emitter, Bare mode only. Straight-line ALU, CP/LD/ST, and
  conditional branches; everything else exits the block. Gate: a measured
  MIPS multiple on doom_bench or a synthetic compute benchmark (target at
  least 2x on the covered subset; if the template tier cannot beat 2x on its
  best case, stop and re-plan).
- J2, hosted correctness. Physical keying, the invalidation bitmap across
  all write sources, inline Sv48 fast-page checks, fault-PC side tables.
  Gate: full asm and C suites green including test_selfmod and the quesOS
  families, plus a hosted MIPS win on the oksh loop.
- J3, quality inside the block. Static register pinning, lazy flags, block
  chaining, backedge interrupt checks. Gate: the 4x-or-better multiple on
  compute-bound workloads, measured against the J0 baselines.
- J4 and beyond, coverage growth by measured frequency, then AArch64.

Each phase is its own card (or small card cluster) with the gate written into
its acceptance criteria; a failed gate stops the stream for re-design rather
than proceeding on hope. This is the same discipline that killed the
predecode tier in one cheap spike instead of a long build.

## 7. Risks, named

- Invalidation completeness is the correctness risk: a missed host-side write
  path (a future device, a new accelerator) silently executes stale code.
  Mitigation: the generation-bitmap check lives inside the memory module's
  write choke points, not at call sites, and test_selfmod plus a new
  bulk-write-over-code fixture pin the contract.
- Compile-time regressions on churny code (a shell forking short-lived
  children whose code compiles and is thrown away) are the performance risk.
  Mitigation: the hotness threshold, physical keying (fork children share the
  parent's already-compiled blocks), and the J0 reuse data before any
  emitter work.
- Precise-fault bookkeeping is the complexity risk; it is scoped to memory
  accesses only, and J1 lands before any of it exists (Bare mode cannot page
  fault).
- Host-platform W^X and cache-coherency differences are a known, bounded
  porting cost, isolated in the arena module.

## 8. Explicit non-goals for this round

There is no optimizing IR tier, no trace formation, no inline caching of
indirect targets, no speculative type or value assumptions, and no attempt to
compile the syscall or privileged surface. The interpreter remains the
semantic authority for every instruction, and every JIT behavior must be
explainable as "the same semantics, faster".
