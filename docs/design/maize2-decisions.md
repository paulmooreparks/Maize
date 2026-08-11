# Maize v2 Decision Document

**Status: working document, Phase 1 of the v2 spec campaign.** Every decision below is PROPOSED until the operator ratifies it, and rulings are folded back in with their dates. The input contract is `maize2-design-brief.md`; nothing here contradicts a brief constraint without saying so out loud. Spec prose (Phase 2) starts only against ratified decisions.

The proposed ratification order follows the dependency structure rather than the numbering: D3 first because flags-versus-flagless cascades into the select instruction, the trap model, and the assembler vocabulary; then D2 and D1, which fix the encoding's raw material; then the rest.

## D3: flags versus flagless

**Status: RATIFIED, flagless. Operator, 2026-08-11.** The v2 base ISA carries no condition register. Comparisons are fused compare-and-branch or compare-into-register; the reasoning below stands as the trail. This unlocks D7's register-condition select, D8's no-flag trap frame, and the D11 condition spellings, and it commits the ALU chapter to a deliberate carry-out design for multi-precision arithmetic.

v1 carries a full condition-flag model (C, N, V, Z, P) in the x86 lineage, with per-instruction flag-effect tables in the spec. The alternative is the RISC-V shape: no condition register at all, with fused compare-and-branch instructions and compare-into-register for materialized conditions.

The case for flagless is unusually strong for this machine in particular:

- The JIT stops emulating flags. Lazy-flag machinery is a classic translator tax, and the v1 JIT already carries it (the `test_flags_lazy_*` fixtures exist precisely because it is subtle). A flagless guest deletes that entire layer, which serves the design-for-translation-first finding from the characterization work.
- The spec shrinks by its single largest per-instruction burden. Every v1 instruction documents its flag effects, and flag-effect completeness was a recurring audit surface. Flagless removes the column from every instruction in the book, which is a direct payment toward the weekend-reimplementable budget.
- Trap entry gets cheaper by construction, since there is no condition state to save and restore. That compounds with D8.
- The float-predicate wart class (SETP without SETNP) cannot recur, because float compares become compare-into-register operations with explicit predicates rather than flag-setting operations with a predicate family.
- Teaching identity stays honest. v1 remains the machine that teaches the x86 flags lineage; v2 teaches the lineage the world converged on.

The honest costs: multi-precision arithmetic loses the carry flag, so ADC/SBB-style code needs carry-out-into-register forms or the RISC-V add-plus-compare idiom, and the ALU chapter must design that deliberately (bignum and crypto workloads are real). Programmers arriving from v1 lose a familiar model. Condition-dense code can be marginally larger.

Consequences if ratified: D7's select takes a register condition rather than a flag condition. D8's trap frame carries no flag state. The D11 vocabulary drops flag-implying spellings (`jump_zero` dies; `branch_eq r3 zero target` is the shape). The v1 SETcc family becomes a small compare-into-register family.

## D2: the register file

**Status: PROPOSED. Recommendation: 32 full-width registers, r0 hardwired to zero, link register at r31, everything else ABI convention.**

The brief fixes 32 registers; the open design is their shape. Proposed: r0 reads as zero and discards writes (the RISC-V x0 device, which gives the ISA free forms for move, negate, compare-against-zero, and discard, and simplifies codegen); r31 is the link register written by the call instruction, since a load-store machine wants branch-and-link rather than v1's pushed return address; the stack pointer is r30 by ABI convention only, with the architecture itself referencing a stack pointer nowhere except the trap model's kernel stack CSR (D8). No sub-register file exists; positional access is the D6 extract/insert design.

Alternatives considered: no zero register (frees one register, loses the idiom compression; rejected because 31 general registers is already past the spill knee), and a pushed-return-address CALL in the v1 style (rejected because it reintroduces an implicit memory write into control flow, which is exactly the class of implicit effect v2 is removing).

Names: `r0` through `r31` canonical, with assembler aliases `zero`, `ra` (r31), `sp` (r30), and the D4 ABI names. Aliases here are register names, not mnemonics, and do not violate the one-canonical-name rule for operations; the disassembler emits the ABI names.

## D1: the base encoding

**Status: RATIFIED as proposed. Operator, 2026-08-11.** The v2 encoding is byte-granular, table-regular, and variable-length, designed for software decode: one opcode byte plus one escape byte per extension page, whole-byte operands, byte-aligned little-endian immediates, length a pure function of the first one or two bytes, reserved opcodes trapping. No bit-packed fixed word, and no compressed form in the base. The escape-page structure becomes D10's opcode-space anchor; D5, D6, and D7 encode against this once D2 fixes the operand byte. The reasoning below stands as the trail.

This is a software machine first. Its two consumers are an interpreter (wants cheap length determination and dispatch) and a JIT (wants regular patterns and dense code, since guest bytes occupy host cache). Hardware-style fixed 32-bit words optimize instruction-fetch parallelism that no shipped Maize realization has, at the cost of bit-field extraction on every operand read and immediate values shredded across fields. v1's byte orientation was the right instinct; its sins were the nibble-packed register cap and irregularity, and those are what v2 fixes.

Proposed shape:

- One opcode byte, with one escape byte opening a second opcode page per extension family. Length is a pure function of the first one or two bytes via a 256-entry table, no other state.
- Operand bytes at byte granularity: a register operand is one byte carrying the 5-bit register number plus 3 bits of operand-form information (the exact split is spec work, but no operand ever shares a byte with another operand).
- Immediates are little-endian, byte-aligned, sized by the operand form, never split.
- Regularity rules stated as spec invariants: every instruction is opcode, then operand bytes, then immediates, in that order; no instruction reads state to determine its own length; reserved opcodes trap (the maize-119 lesson, structural in v2).

The honest tradeoff: a variable-length encoding is less friendly to a hypothetical hardware realization than fixed words, though Thumb-2 and x86 prove variable-length hardware is routine. The FPGA dream stays on the realization shelf, and the machine that exists is software; the encoding should serve the consumers it has. A compressed 16-bit form is explicitly not part of the base (density is already good at byte granularity) and remains available as a future extension page if measurement ever justifies it.

## D5: width-modifier semantics

**Status: PROPOSED. Recommendation: `.w` operations zero-extend their 32-bit result into the full register; width-modified ALU forms exist for `.w` only; `.b` and `.h` exist only on loads, stores, extract, and insert.**

Zero-extension of 32-bit results is what both JIT host architectures do natively (x86-64 32-bit operations zero the upper half, and AArch64 w-register writes do the same), so `.w` arithmetic lowers to a bare host instruction with no fixup. The RISC-V alternative (sign-extending 32-bit results) costs an explicit extend on both major hosts and buys nothing the compiler needs, since C's `int` semantics are satisfied either way when compares are width-consistent.

Restricting ALU width modifiers to `.w` is a deliberate ISA-size cut: C promotes narrow types to `int` before arithmetic, so byte and halfword ALU forms are dead weight the backend would rarely emit. Narrow widths live where they are real, on the memory operations (`load.u8`, `load.s16`, `store.b`, and their kin) and on extract/insert.

## D6: positional extract and insert

**Status: PROPOSED. Recommendation: as designed in the brief, with immediate-only positions in the base.**

A dotted register as a source extracts, producing a fresh zero-extended full-width value (with a sign-extending variant), and reads nothing back into the source. A dotted register as a destination inserts, an explicit read-modify-write that is the only merge site in the ISA. The dotted forms name the eight bytes, four quarters, and two halves (`r3.b5`, `r3.q2`, `r3.h1`). Behind them sit general bitfield extract and insert with immediate bit position and width, of which the dotted forms are aligned shorthands. Register-driven position and width are excluded from the base and wait for evidence, per the selection rule. Flag effects are moot under D3.

The brief's boundary is restated as a spec invariant: width modifiers ride memory operations, positional dots ride register operands of extract and insert, and no memory operation ever targets a register slice.

## D7: the select instruction

**Status: PROPOSED. Recommendation: a conditional-move pair with a register condition, matching the host cmov shape.**

Under flagless D3, select takes its condition from a register. The four-operand general form (`rd = rc ? rt : rf`) encodes wide and lowers to two host instructions; the proposed shape is instead a destructive three-operand pair, `select_nz rd rc rs` (rd becomes rs when rc is nonzero, else unchanged) and `select_z`, which is exactly the host cmov contract on both x86-64 and AArch64 and composes into the general form in two instructions when needed. The v1 lesson rides along: the opcode is inert until the backend recognizes diamond-shaped selects, so the qbe (and later LLVM) select-recognition work is part of the same deliverable, not a follow-up hope.

## D8: the trap model

**Status: PROPOSED. Recommendation: a four-word hardware-pushed frame on a kernel-stack CSR, vectored causes, syscall arguments live in registers, and no general-purpose register saved by hardware.**

The v1 pain this kills is the thirteen-PUSH prologue repeated per handler. Proposed: on any trap the machine switches to a kernel stack named by a CSR, pushes exactly four words (pc, status, cause, aux), and vectors by cause. General-purpose registers are the kernel's problem, calling-convention-aware: the syscall path saves only what it uses, and a full register save happens only on an actual context switch. Under flagless D3 there is no condition state to preserve, so the hardware frame is genuinely minimal. Syscall arguments arrive in the D4 argument registers and stay live across the trap boundary; the return value lands in place; a single return instruction pops the frame and resumes.

Fault restartability is a spec-wide contract: a faulting instruction resumes cleanly after the kernel services the fault, and multi-step operations (the block-memory family) define their visible mid-operation register state, the maize-331/FEAT_MOPS pattern with the maize-194 restart discipline as the implementation template.

The characterization pressure is recorded here explicitly: syscall-bound code gains only 1.8x under the v1 JIT because SYS ends a compiled block. The v2 trap design should let the JIT treat a syscall as a call-shaped boundary rather than a compilation wall where feasible; the mechanism is implementation, but the spec must not forbid it.

## D12: the privileged architecture

**Status: PROPOSED. Recommendation: carry v1's Sv48 paging and two-level privilege forward nearly unchanged; move control registers into a numbered CSR space.**

Sv48 translation, the software TLB model, page-fault delivery, and supervisor/user separation are proven on v1 and quesOS's paging code ports almost verbatim if the page-table format stays identical, which is the proposal. Changes are limited to what other decisions force: the CR file generalizes to a numbered CSR space with `csr_read`/`csr_write` (making room for the D8 kernel-stack CSR, the D10 feature CSRs, and future extension state), TLB maintenance keeps its two operations under D11 naming, and encoding room is reserved for a third privilege level without defining one.

## D13: syscall-ABI continuity

**Status: PROPOSED. Recommendation: keep the trap shape recognizably v1; replace SETSYSG/CLRSYSG opcodes with a CSR bit.**

The SYS instruction, its cause, the syscall number's register, and arguments-in-registers all stay shaped as in v1, so the quesOS port is the promised recompile plus a mechanical trampoline rewrite, and both ABIs (Linux-compat and native) carry over undisturbed. The one cleanup: the provider-select flag becomes a CSR bit rather than two dedicated opcodes, since dedicating opcode space to a mode toggle was always encoding extravagance, and the shrinking native provider makes the toggle progressively less load-bearing anyway.

## D4: the calling convention

**Status: PROPOSED. Recommendation: a RISC-V-flavored convention sized to 32 registers.**

Eight argument registers that are also the return registers (arguments r2 through r9, results in r2/r3), roughly ten callee-saved registers, the rest caller-saved temporaries, `ra` at r31, `sp` at r30, 16-byte stack alignment (headroom for a future vector extension), no red zone, small structs up to two words passed in registers and larger ones by reference, and varargs entirely on the stack past the register arguments. The full ABI supplement is Phase 2 prose; the decision fixes the register-role map and the alignment so D1's encoding examples and the backend plan can proceed.

## D9: memory architecture details

**Status: PROPOSED. Recommendation: as the brief states, with a boot-information block for discovery.**

Little-endian only. Guest physical memory is bounded and configurable, discovered through a boot-information block the VM places in memory plus a CSR reporting its address, rather than probing. Misaligned access stays defined-allow (the v1 stance, kept deliberately as a teaching-friendly simplification with a documented performance note). Naturally aligned accesses up to eight bytes are single-copy atomic; misaligned accesses carry no atomicity guarantee. The implementation notes from the brief (flat host reservation, inline-TLB JIT loads, dirty-page tracking, the reserved capability tag plane) ride along as non-normative appendix material.

## D10: extension governance

**Status: PROPOSED. Recommendation: named, versioned extensions with CSR-discoverable presence and per-extension opcode pages.**

The base freezes once, forever. Each extension has a name (`base`, `cap`, `vec`, `meter`, `atomic` as the anticipated first set), an independent version, an allocated opcode page behind the D1 escape byte, and any state it adds lives in its own CSR range. Discovery is a feature CSR (a presence bitmap for fast checks) plus an extension list in the D9 boot-information block carrying versions. A conformance claim names the base version plus the exact extension set, and the conformance suite is factored the same way, so a base-only VM is a complete, certifiable machine. Nothing in reserved space ever executes as a no-op; unknown opcodes trap, which is what makes forward compatibility testable.

## D11: the assembler conventions

**Status: PROPOSED, with several sub-questions already settled in direction by the operator (see the brief).**

Settled and restated: full lowercase word mnemonics; underscores spell compound names; the dot is reserved for operand structure, surviving inside names only as the possible length-specifier form; one canonical name per operation with the alias table deferred; the `@` memory sigil and the digit-separator flexibility carry over.

The three genuinely open calls, with recommendations:

- Operand order: destination-first (`add r3 r1 r2` meaning r3 = r1 + r2), breaking with v1's source-to-destination. The machine now teaches the RISC lineage, every assembler a v2 reader will meet is destination-first, and destination-first reads as assignment, which suits word mnemonics. Comma-free layout is kept; the order changes, the voice does not. The counterargument is v1 muscle memory, and it is real but small against an audience of newcomers and code generators.
- Decimal marking: bare decimal becomes the default, with `$` hex and `%` binary staying mandatory for their bases and `#` retired. Word mnemonics plus named registers make bare decimal unambiguous to a reader, and the always-marked discipline's benefit shrinks once the only unmarked base is the one humans assume anyway.
- Case: lowercase is canonical and the assembler is case-sensitive, keeping exactly one spelling per program element, which is the same single-source discipline as the no-alias rule.
- The length-specifier spelling, the one sanctioned maybe-dot: keep the dot (`load.u8`, `store.w`), because under D5 the dotted surface is small (memory operations plus `.w` arithmetic), the dot makes the width family visible as a family, and it keeps compound-name underscores unambiguous as word separation.

## Interactions ledger

Ratifying D3 as flagless unlocks D7's register-condition select, D8's minimal frame, and the D11 vocabulary. D2 and D1 together fix the operand byte, which D5, D6, and D7 encode against. D8 and D12 jointly define the CSR space, and D13 rides on both. D4 threads through D8 (which registers stay live across a syscall trap). D10's opcode pages depend on D1's escape structure. D11 depends on D3 for spellings and can otherwise ratify independently.
