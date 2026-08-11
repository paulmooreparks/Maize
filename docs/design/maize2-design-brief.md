# Maize v2 Design Brief

**Status: Phase 0 input contract, 2026-07-24.** This document is not the v2 specification and it makes no decisions. It assembles the requirements, evidence, and open questions accumulated across the 2026-07-23/24 design sessions so that Phase 1, the decision document, starts from the record rather than from a blank prompt. Where this brief and a later ratified decision disagree, the decision wins. The campaign plan, tiers, and gates live in the "Maize v2 (the next machine)" workstream on the board.

The operating premise is that v1 is unreleased, so a clean break costs as little now as it ever will. v2 is designed as the machine the platform vision actually wants (deterministic, forkable, meterable, capability-ready, snapshot-native) rather than as a compatibility descendant. v1 remains what it is: the CISC teaching machine, complete with its frozen spec, and the source of the lessons below.

## Why a clean break, and what only a break can fix

Most of what we want from v2 could be added to v1 as extensions. Three things cannot, because they live in the operand byte and the register model, and they are the reason v2 exists as a separate ISA rather than as v1.2:

- The register cap. v1 packs the register ID into a nibble of the operand byte, so the machine is saturated at 16 registers forever. v2 targets 32, which is where spill pressure mostly disappears and what the whole industry converged on.
- Decode regularity. v1's operand-byte scheme makes decode branchy for the interpreter and pattern-irregular for a JIT. v2 wants a regular base encoding that both a dispatch loop and a translator find cheap.
- The sub-register model. v1's merge-on-write sub-registers produced a family of warts we hit repeatedly: the LDZ/LDS asymmetry, merge-versus-extend confusion, partial-register dependencies. v2 replaces the model rather than patching it.

Everything else in this brief could in principle have been a v1 extension, and lands in v2 because the break is happening anyway.

## Structural requirements

### A modular base plus named extensions

v2 adopts the RISC-V structural lesson. A small mandatory base is frozen once and never thawed. Everything else (capabilities, vectors, metering, atomics, future additions) arrives as a named, versioned, optional extension. The freeze-and-thaw ceremony that consumed the v1.1 planning cycle must be structurally impossible to need again. The base must define how a program discovers which extensions a machine implements.

The weekend-reimplementable constraint is a hard budget, not a slogan. A conforming base VM must remain something one person can build against the spec and the conformance suite in a weekend. Every base-ISA candidate pays rent against that budget.

### Load-store, with width on the operation

v2 is a load-store machine. Only loads and stores touch memory, which collapses the fault-restart complexity that cost real work in v1 (PUSH/CALL restart plumbing in the MMU rounds, the restartability design in the block-memory instruction plan). This is a deliberate identity change, ratified by the operator: v1 stays the CISC that teaches the x86 lineage, and v2 is the machine the world converged on.

Registers are full-width and plain. Width lives on the operation as a modifier, with one rule everywhere: `.b`, `.h`, and `.w` mean an 8, 16, or 32-bit access, and a bare mnemonic means 64. Loads state their extension explicitly (the LDZ/LDS pair as modifiers, so the pair is regular by construction and the v1 asymmetry cannot recur). The 32-bit ALU forms cover C's `int` semantics with a defined result in the full register.

### Positional extract and insert, in place of merge semantics

v1's dotted sub-registers did two jobs. The bad job, implicit merge on every narrow write, dies with the sub-register model. The good job, naming any byte, quarter, or half at any position, survives as explicit operations: a dotted register as a source extracts (zero-cost, no dependency), a dotted register as a destination inserts (an explicit read-modify-write, the only place merge exists). General bitfield extract and insert (BFX/BFI over arbitrary position and width) back the aligned dotted forms. The design intent is that the common path is always full-width and clean, and merge happens only where the programmer visibly asked for it.

The boundary between the two dot mechanisms is explicit, and Phase 1 must preserve it (operator-confirmed 2026-07-24). Width modifiers belong to memory operations and describe the access width; a load always extends into the full destination register and never targets a register slice, because a load that merged into a slice would be exactly the implicit memory-op merge v2 exists to kill. Positional dotted forms belong to register operands of the extract and insert operations only, and they name any of the eight bytes, four quarters, or two halves (in the shape of r3.b5, r3.q2, r3.h1), so v1's full positional reach survives intact. Placing a loaded byte at an interior position is deliberately a two-step sequence, a width-modified load followed by an explicit insert.

## Instruction-set requirements, with their evidence

The v1.1 thaw investigation (card maize-340 and its audit trail) is the evidence base, and its selection rule carries over: an instruction earns base-ISA space if the compiler backend demonstrably synthesizes it, if it is a standard primitive with a concrete coming need, or if it completes a symmetry. App-shaped operations never earn ISA space; the $F3 palette-blit lesson is the standing counterexample.

- Sign- and zero-extending loads are base, as the width-modifier scheme above, closing the LDZ/LDS asymmetry structurally.
- Conditional select (the CMOV question) is base, but its shape depends on the flags decision (D3 below), and the v1 lesson stands: the instruction is inert without backend select-recognition, so the toolchain work is part of the same deliverable.
- Block memory copy and set are base instructions, not syscalls, per the boundary policy. They must be specified restartable under paging with defined mid-operation register state, in the ARM FEAT_MOPS shape. The overlap semantics decision (memmove-safe versus forward-only variants) is explicit, not implied.
- The float-predicate completeness gap found by the codegen audit (SETP without SETNP, no ordered-equal predicate) must not recur; whatever the v2 compare/predicate scheme is, it is complete by construction.
- BSWAP or an equivalent byte-reverse belongs in the base on the strength of the committed networking milestone, cheap and single-slot.
- Rotates, CLZ/CTZ/POPCNT, integer MIN/MAX/ABS, and bit test/set/clear stay out of the base. The audit found no synthesized demand. They are early extension candidates the moment a workload produces evidence.
- Trap entry must be cheap by design (the problem PUSHALL/POPALL was filed to patch). Whether that means a hardware-stacked minimal frame, banked registers, or a save-list instruction is a Phase 1 decision; the requirement is that the v1 thirteen-PUSH prologue has no v2 equivalent.

The non-additive v1 wart classes must be impossible or fixed from day one: no operand read whose width silently disagrees with its operand's encoding (the maize-229 class), no invalid encoding that silently defaults instead of trapping (the maize-119 class), and no instruction the VM executes that the assembler cannot emit (the maize-202 class; the grammar-mirrors-tokenizer discipline plus surface-completeness checks are the standing cure).

## Machine architecture requirements

### Memory

The architecture keeps what v1 got right: a flat 64-bit virtual byte-addressable space, little-endian, defined-allow misalignment, and the no-undefined-behavior trap model. v2 additionally defines guest physical memory as bounded and configurable, with the 64-bit space reached through translation, saying plainly what v1 only implied.

Implementation notes recorded for the v2 VM (internally replaceable, not spec): a single flat host reservation replaces v1's sparse 256-byte block map, letting the host MMU provide sparseness; the translation path is designed for an inline-TLB JIT load (tag check, add, load); dirty-page tracking via host page protection (or GetWriteWatch on Windows) is first-class so incremental snapshot and copy-on-write fork fall out of the representation; guard-page bounds tricks live behind the host-backend seam with a portable-C fallback; and a parallel tag plane is reserved in the design so the capability extension can attach unforgeable tags without a representation change.

### The machine as a value

The entire architectural state is defined in the spec as one enumerable vector with no hidden state. The snapshot set is the architecture by definition, which prevents the v1 failure where snapshot completeness had to be reverse-engineered after the MMU landed. Nondeterministic inputs (clock, entropy, network, console) exist only at the device boundary as recordable inputs.

### Cost model separation

v1's separation is kept: behavioral conformance never depends on timing, and the base spec carries no cycle counter. Metering is a first-class optional extension, defined precisely enough that cycle budgets, fair scheduling, and accounting work when it is present and are invisible when it is not.

### Privileged architecture

The trap/interrupt model, the CR file, and paging carry the v1 lessons (Sv48-shaped translation worked; the choke-point access-kind discipline from the MMU reviews is kept). Whether v2 keeps the exact Sv48 format or revises it is a Phase 1 decision, weighed against quesOS porting cost. The syscall trap shape (cause-7 plus the provider-select flag) should stay recognizably compatible so the quesOS port is a recompile plus a thin trampoline rewrite, not a redesign; this also preserves the quesito-tier and two-ABI plans unchanged.

### Capabilities and vectors are extensions, not base

Parity comes first on a clean load-store base. The capability extension (CHERI-shaped: bounds and permissions in the pointer, unforgeable tags) is the first planned extension, and the base encoding must leave it room, including the pointer format headroom and the memory tag plane. Vectors follow later as a length-agnostic extension. Neither is in the parity estimate.

## Assembler requirements

The house voice is kept, because it is ISA-independent and good: always-explicit numeric bases (`$` hex, `%` binary, `#` decimal), flexible digit separators (backtick, underscore, comma), the `@` sigil on every memory access, comma-free space-separated operands, and the tooling discipline (grammar mirrors the tokenizer as a single source of truth, machine-readable diagnostics, editor-integration modes).

The vocabulary is redrawn with the ISA, and the dotted notation is repartitioned rather than removed: width modifiers ride memory operations, positional dotted register operands ride the extract and insert forms, and the CISC addressing forms go with the CISC. Four conventions are consciously re-decided rather than inherited, and are Phase 1 decisions: operand order (v1's source-to-destination comma-free style versus the destination-first order every RISC-lineage assembler uses); whether decimal stays marked with `#` or becomes the bare default; mnemonic register (terse abbreviations in the v1 style versus full lowercase words in the LLVM-IR and WebAssembly-text style, which is the direction every machine-first text format designed after the teletype era chose, raised by the operator 2026-07-24 on the grounds that compilers and models emit assembly and humans mostly read it while debugging); and case policy. Whatever the mnemonic decision, the disassembler and any formatter emit exactly one canonical name per operation. Whether a blessed short-alias table exists alongside the canonical names (the PowerShell Write-Host/echo model, operator-raised 2026-07-24) is an open sub-question, with the tradeoffs on record: aliases in PowerShell serve an interactive prompt that assembly does not have, and that ecosystem needed formatter and linter policing to keep aliases out of committed code, but the mitigations are known (canonical-only tool output, normalize-on-format). The recorded recommendation is one canonical name at v2 launch, with the alias table deferred, because it is a purely additive assembler-layer feature with no ISA content and can be added later from felt friction without foreclosing anything. Full-word length is a cold-tail concern only: the hot instructions are short as words (load, store, add, jump), and the wordiest realistic mnemonics (in the shape of fused.multiply.subtract, tlb.invalidate.all, or a future atomic.compare.exchange) belong to rare operations that can afford the length, with pragmatic abbreviation at the extreme tail (the WebAssembly cmpxchg precedent) a legitimate tool. The mnemonic decision interacts with D3 (a `jump.zero` spelling presumes flags) and with the width-modifier scheme (full words allow the extension pair to fold into the modifier, in the shape of `load.u8` and `load.s8`).

On a load-store machine the `@` sigil gains force: it appears exactly where memory is touched and nowhere else. The assembler spec should treat that as a design feature.

## Implementation strategy

The v2 VM and tools are written as a single portable-C codebase from day one. The tools (assembler, linker, disassembler) are written in the C subset the Maize toolchain itself compiles, so they self-host as guest programs once the backend retargets; the toolchain and the tools co-evolve, with the toolchain extended where a tool needs it. The VM is portable C but compiled for production by each host's optimizing compiler; it does not self-host, and the existing Linux and Windows host toolchains (including Windows PGO) are kept as-is. macOS joins as a host at parity, since portable C makes it nearly free.

The JIT is designed with a front-end/back-end seam from the start: Maize decode on one side, host codegen on the other, so x86-64 and AArch64 (Apple Silicon, with its MAP_JIT/W^X discipline) are two bounded backends behind one interface rather than two JITs. The v1 JIT's host-codegen half is expected to carry over across that seam; its measured results are a Phase 1 input.

The mzvm/quesOS boundary policy applies to v2 from its first commit: the VM is instruction execution, MMU mechanism, trap delivery, devices behind a swappable host-backend interface, and a minimal boot path, with the embedded quesito ROM tier as the default boot payload and quesOS as the full OS. No OS policy enters the v2 VM, which also keeps the eventual portable reference VM story cheap.

## Migration and parity

quesOS, the userland, the demos, and the C test corpus carry over by recompile, and the quesOS port (boot stub, trap trampoline, paging code, context switch) is the designated portability audit for the kernel's machine-facing seam. Parity means: the v2 spec ratified; VM, assembler, linker, disassembler, and backend working; the conformance approach in place; quesOS booting to the shell with the userland; DOOM and kilo running. The estimate on record is two to four agent wall-clock weeks after ratification, planning number three, with the compiler backend as the longest pole and operator ratification latency on the critical path.

## Open decisions for Phase 1

Each item below becomes a ratifiable entry in the decision document, with tradeoffs and a recommendation. The list is the contract for Phase 1's completeness:

- D1: the base encoding scheme (instruction width, field layout, whether a compressed form exists and how it nests, extension opcode space reservation).
- D2: the register file (count, names, a hardwired zero register or not, which registers are special and why).
- D3: flags versus flagless (a condition-code register in the x86/ARM lineage, or compare-and-branch in the RISC-V lineage); this decision shapes select, the predicate family, and multi-precision arithmetic.
- D4: the calling convention and ABI (argument registers, callee-saved set, stack discipline, the compiler-runtime function set).
- D5: precise width-modifier semantics (what a `.w` operation writes into the upper 32 bits, and whether that matches x86-64 zeroing or something else).
- D6: extract/insert and BFX/BFI semantics, including flag effects if D3 keeps flags.
- D7: the select instruction's shape (condition source, one form or a family).
- D8: the trap model and trap-frame format, including the cheap-trap-entry mechanism and what IRET restores.
- D9: the memory architecture details (physical size model and its discovery, misalignment cost posture, atomicity of naturally aligned accesses).
- D10: extension governance (naming, versioning, the discovery mechanism, what conformance claims look like per extension).
- D11: the assembler conventions (operand order; decimal marking; mnemonic register, terse versus full words; whether a blessed short-alias table accompanies the canonical names or arrives later, if ever; case policy).
- D12: the privileged architecture (paging format, CR file, privilege levels, and how close to v1's Sv48 it stays).
- D13: syscall/trap ABI continuity with v1's cause-7 and provider-select shape, and what the quesOS trampoline rewrite is allowed to cost.

## Gates and sequencing

Phase 1 may start immediately, but the encoding decisions (D1, and the D5/D6 details that ride on it) do not freeze until the v1 JIT characterization lands (the maize-343 report and the maize-354 paging-tax re-characterization), because the JIT is the best available evidence for what a translator wants from an encoding. Everything in the hybrid plan after Phase 1 is gated on ratified decisions, not on calendar.

While the campaign runs, the v1.1 thaw (maize-340) does not execute, and maize-331, maize-272, and maize-333 are held; their formal supersession awaits the operator's v2 ratification at the end of Phase 1. quesOS maturation continues in parallel throughout, because kernel work is ISA-agnostic and transfers by recompile.

## Non-goals

- v1 binary compatibility is not a v2 requirement. The world recompiles. The dual-front-end-one-backend design (two ISA personalities over one JIT backend) is recorded as a viable later option if "old binaries run forever" ever becomes a product promise, and nothing in v2's design may foreclose it, but it is not built now.
- The quesOS-on-x86/Hyper-V port is not part of this campaign. The v2 recompile is the portability audit; mzvm ports are the runs-everywhere story. Revisit after 1.0, after storage exists, and after the v2 port has carved the kernel's machine seam.
- Capabilities, vectors, and metering are not in the parity target. They are the first extensions, in roughly that order, and parity must not wait on them.

## Sources

The reasoning trail behind every requirement above lives in: docs/design/mzvm-quesos-boundary.md (the boundary policy and its audit), docs/design/quesos-enlightened-layer.md (the platform vision the machine serves), the maize-340 card and comments (the thaw inventory and the codegen evidence), the maize-331/272/229/119/202 cards (the instruction and wart inventory), the maize-338 card (the quesito two-tier design), the maize-343/354 cards (the JIT characterization gate), and the 2026-07-23/24 design-session decisions recorded in the project memory and the workstream notes.
