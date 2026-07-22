# JIT: Tier-Up of Hot Blocks from the Interpreter

Status: design, revised after adversarial review (maize-181 comment #3178).
Umbrella card: maize-181.
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

The percentages are callgrind_annotate SELF (exclusive) Ir shares, so the
rows do not double-count one another; `tick()`'s row excludes the time spent
inside the helpers it calls.

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
blocks, and the check keys on PHYSICAL addresses, which is where the
platform's conventions make every writer visible. Guest stores reach memory
through the memory module's write seams after `translate()` has already
produced a physical address. Host-side writers (the $F3/$F4/$F5 native
accelerators, the hostfs read and getdents and stat copies in `src/sys.cpp`)
receive flat PHYSICAL addresses by platform convention: under quesOS the
guest kernel translates user VAs before forwarding (the maize-247
`do_bulk_*` pattern), and a bare-mode guest's addresses are identity. The
adversarial review verified there are exactly three write seams in today's
code, not one, so the generation-bitmap check is installed at each of the
three, and adding a fourth seam without the check is the named regression
risk in section 7. A per-page generation bitmap makes the check one test on
the write path for pages with no compiled code, which is almost all of them.
The review also found that the framebuffer device only READS guest memory
(`mm.read_into` in `src/devices.cpp`); no device-DMA write path into guest
memory exists today, and this document stops claiming one.

The committed fixture `asm/test_selfmod.mazm` pins the architectural contract
but executes its target only twice, far below any plausible hotness
threshold, so as it stands it never exercises a compiled block. J1 ships a
hot variant through the guest-store seam (execute the target past the
threshold, patch it, and verify the re-executed result, with live chained
predecessors exercising the unlink contract below); J2 extends the hot
variant through the two host-side write seams.

### 3.2a Chained jumps unlink on invalidation

Direct chaining patches a block's exit jump to its compiled successor, and
that patched link is a second copy of the successor's identity that the
page-generation bitmap alone cannot see (the known QEMU tb_invalidate
gotcha the review raised). Every block therefore keeps a list of its
incoming chain links, and invalidating a block walks that list and restores
each predecessor's exit to the lookup stub before the block is discarded.
Chaining and unlinking land together as one unit; a chained successor is
never created before its unlink bookkeeping exists. The J0 survey moved
that unit forward from J3 into J1: measured blocks are short (median 2 to 7
instructions) and 77 to 83 percent of dynamic exits are direct, so an
unchained J1 would pay a dispatcher round trip every few instructions and
could not meet its own end-to-end gate.

### 3.2b Indirect transfers go through the central lookup

Returns, calls through registers, and computed jumps cannot chain
statically, and the cc-self-compile workloads this campaign targets are
full of them. Their path is the central block table: an open-addressed hash
on (physical entry, privilege), sized for the code cache's block budget,
probed by a short inline sequence at each indirect exit. J0 measures the
dynamic frequency of indirect transfers alongside block shape so the cost
model for this path rests on data; if indirect exits dominate, a small
per-site last-target cache (one compare before the hash probe) is the
planned second step, and it is cut from J3 if the J0 numbers say it is not
needed.

### 3.3 Interrupts and the timer stay boundary-precise

The interpreter checks interrupt delivery at instruction boundaries;
compiled code checks at block exits. A block ends at EVERY control transfer,
backward ones included, so no block contains an internal loop and the
"backedge check" is simply the exit check of a block whose chain target lies
behind it; coverage numbers are unaffected because a hot loop body is a hot
block re-entered through its chain. The instruction-count timer decrements
against a budget in the CPU context, so a 250,000-instruction slice
(maize-319) spans many blocks and the per-block cost is one decrement and
branch. Delivery granularity coarsens from one instruction to one block,
bounded by the block-size cap at 512 guest instructions (and by the page
boundary, whichever is first): worst-case added interrupt latency is 512
guest instructions, under ten microseconds at 60 MIPS, invisible next to
the 4 ms timer slice and irrelevant to the IRQ-driven console and keyboard
paths, which are already only boundary-precise today.

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
- J1, Bare-mode emitter (J3 merged in by the operator, 2026-07-23). The first
  build shipped a call-threaded emitter (one host call to a semantic thunk per
  guest instruction) and measured 0.95x: it removed dispatch, which J0 and the
  maize-307 spike proved is roughly zero percent of DOOM, while keeping the
  handler-body cost and adding a call per instruction. That measured kill-gate
  failure moved J3's quality work forward into J1: the emitter now inlines the
  hot shapes directly as host code (guest state addressed off a pinned anchor
  register, the L1 memory fast path inlined, flags materialized inline, and a
  compare-and-branch fusion for the dominant ALU-then-Jcc loop enders), keeping
  the thunk tier only as the fallback for shapes it does not cover. Direct-exit
  chaining with unlink and the central indirect-lookup probe are in scope. The
  differential checker (--jit-check, every compiled block verified against the
  interpreter oracle) is the standing miscompile net for this and every later
  phase. Gate: at least 2x END-TO-END wall-clock on doom_bench headless with the
  covered dynamic-instruction fraction reported alongside; not satisfiable by a
  microbenchmark. INSTRUMENT NOTE (2026-07-23): the historical real-time-paced
  doom_bench is throughput-saturated. It waits on the host clock through
  DG_SleepMs to pace DOOM's 35 Hz tic rate, so past a modest interpreter speed
  its frame time is pinned to wall-clock pacing, not execution, and a JIT
  throughput win is invisible in it (measured 0.97x, with ~53 percent of samples
  in the guest's real-clock spin). doom_bench built -D BENCH_DETERMINISTIC uses
  the maize-251 virtual clock (the selfcheck's mechanism): DG_SleepMs advances a
  virtual counter with no real wait, so the bench executes 120 real DOOM frames
  as fast as the VM allows and its own boot/frame timing (real host time) then
  measures pure throughput. That is the throughput instrument for this gate.
- J2, hosted correctness. Physical keying, the invalidation bitmap across
  all write sources, inline Sv48 fast-page checks, fault-PC side tables.
  Gate: full asm and C suites green including test_selfmod and the quesOS
  families, plus a hosted MIPS win on the oksh loop.
- J3, folded into J1 (2026-07-23). Its register-pinning and lazy-flags-in-codegen
  work is the inline emitter J1 now ships; what remains for a later card is
  deepening it (a persistent register allocation across chained blocks, wider
  operand coverage) rather than introducing it.
- J4 and beyond, coverage growth by measured frequency, then AArch64.

Each phase is its own card (or small card cluster) with the gate written into
its acceptance criteria; a failed gate stops the stream for re-design rather
than proceeding on hope. This is the same discipline that killed the
predecode tier in one cheap spike instead of a long build.

## 7. Risks, named

- Invalidation completeness is the correctness risk: a missed host-side write
  path (a future device, a new accelerator, a fourth write seam added without
  the check) silently executes stale code. Mitigation: the generation-bitmap
  check lives inside the memory module's three existing write seams, not at
  call sites; the J2 hot-selfmod fixture patches code through each seam in
  turn; and the seam count is asserted in review whenever the memory module
  changes.
- Chain-unlink bookkeeping is the second correctness risk, because a stale
  chained jump bypasses the block table entirely. Mitigation: chaining and
  unlinking are one J1 unit, and the J1 hot-selfmod fixture invalidates a
  block that has live predecessors.
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
