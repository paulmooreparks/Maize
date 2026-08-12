# Maize v2 Decision Document

**Status: COMPLETE. All thirteen decisions and the terminology ruling are ratified, operator, 2026-08-11.** This document is now the v2 decision record: Phase 2 chapter drafting proceeds against it, and any change to a ratified decision is a new dated ruling here, never a silent edit. The input contract was `maize2-design-brief.md`; two recommendations were overruled during ratification (D11 operand order and D11 decimal marking, both in the operator's direction), and every other decision was ratified as proposed.

**The specification is ratified and base 2.0 is frozen. Operator, 2026-08-12.** The text in `docs/spec-v2/` is the ratified specification, and the base freezes with it: no instruction is added to the base, no encoding in the base changes meaning, and no reserved byte in the base is ever assigned. Every later capability arrives as a named, independently versioned extension, and a correction to the ratified text is an erratum under the versioning chapter's rule, which changes no conforming machine's behavior. Three addenda were ratified after the original thirteen and are recorded below in place: D11 on immediate-move width explicitness, D8 on the trap-entry scratch register and `csr_swap`, and D10 on page-less extensions.

The proposed ratification order follows the dependency structure rather than the numbering: D3 first because flags-versus-flagless cascades into the select instruction, the trap model, and the assembler vocabulary; then D2 and D1, which fix the encoding's raw material; then the rest.

## Terminology ruling: the literal word

**Status: RATIFIED. Operator, 2026-08-11.** Maize uses the literal, classical size vocabulary, as v1's sub-register letters already did: a **word** is the machine's native 64 bits, a **half-word** is 32, a **quarter-word** is 16, and a **byte** is 8. The width letters follow directly, so `.b`, `.q`, and `.h` name byte, quarter-word, and half-word, a bare mnemonic operates at the full word, and the positional forms `r3.b5`, `r3.q0` through `r3.q3`, and `r3.h0`/`r3.h1` are the same letters indexed by position. This is deliberately not the Intel convention (word frozen at 16 by 8086 compatibility) nor the RISC-V and ARM convention (word frozen at 32), both of which are historical accidents preserved for compatibility that v2 does not carry. The spec's terminology chapter states this up front, precisely because every reader arrives with one of the frozen conventions in their head. Earlier drafts of D5 and the brief briefly imported the RISC-V letter meanings; the operator caught the collision and this ruling ends it.

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

**Status: RATIFIED as proposed. Operator, 2026-08-11.** Thirty-two full-width registers. r0 is hardwired zero (reads zero, discards writes), r31 is the link register written by the call instruction, the stack pointer is r30 by ABI convention only, and the architecture references a stack pointer nowhere except the D8 kernel-stack CSR. No sub-register file. Canonical names r0 through r31, with zero, ra, sp, and the D4 ABI names as register aliases, and the disassembler emitting the ABI names. The operand byte's register field is therefore 5 bits, which D5, D6, and D7 encode against. The reasoning below stands as the trail.

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

**Status: RATIFIED as proposed. Operator, 2026-08-11.** Half-word `.h` operations zero-extend their 32-bit result into the full word; width-modified ALU forms exist for `.h` only; `.b` and `.q` exist only on loads, stores, extract, and insert, with `z` and `s` naming load extension (`load.zb`, `load.sq`; bare `load`/`store` at the word). The reasoning below stands as the trail.

All width letters follow the terminology ruling above: `.h` is the 32-bit half-word, and a bare mnemonic is the full 64-bit word.

Zero-extension of half-word results is what both JIT host architectures do natively (x86-64 32-bit operations zero the upper half, and AArch64 w-register writes do the same), so `.h` arithmetic lowers to a bare host instruction with no fixup. The RISC-V alternative (sign-extending 32-bit results) costs an explicit extend on both major hosts and buys nothing the compiler needs, since C's `int` semantics are satisfied either way when compares are width-consistent.

Restricting ALU width modifiers to `.h` is a deliberate ISA-size cut: C promotes narrow types to `int` before arithmetic, so byte and quarter-word ALU forms are dead weight the backend would rarely emit. Narrow widths live where they are real, on the memory operations (`load.zb`, `load.sq`, `store.b`, and their kin, with `z` and `s` naming the extension) and on extract/insert.

## D6: positional extract and insert

**Status: RATIFIED as proposed. Operator, 2026-08-11.** Dotted source extracts to a fresh full-width value (zero- or sign-extending variants), dotted destination inserts as the ISA's only merge site, all fourteen positional forms reachable, general bitfield extract and insert with immediate position and width behind them, register-driven positions excluded from the base pending evidence. The boundary invariant holds: width modifiers ride memory operations, positional dots ride register operands of extract and insert, and no memory operation targets a register slice.

A dotted register as a source extracts, producing a fresh zero-extended full-width value (with a sign-extending variant), and reads nothing back into the source. A dotted register as a destination inserts, an explicit read-modify-write that is the only merge site in the ISA. The dotted forms name the eight bytes, four quarters, and two halves (`r3.b5`, `r3.q2`, `r3.h1`). Behind them sit general bitfield extract and insert with immediate bit position and width, of which the dotted forms are aligned shorthands. Register-driven position and width are excluded from the base and wait for evidence, per the selection rule. Flag effects are moot under D3.

The brief's boundary is restated as a spec invariant: width modifiers ride memory operations, positional dots ride register operands of extract and insert, and no memory operation ever targets a register slice.

## D7: the select instruction

**Status: RATIFIED as proposed. Operator, 2026-08-11.** The destructive conditional-move pair with a register condition, `select_nz rd rc rs` and `select_z`, lowering one-to-one to host cmov on both JIT targets, with backend select-recognition shipping as part of the same deliverable so the instruction is exercised from day one.

Under flagless D3, select takes its condition from a register. The four-operand general form (`rd = rc ? rt : rf`) encodes wide and lowers to two host instructions; the proposed shape is instead a destructive three-operand pair, `select_nz rd rc rs` (rd becomes rs when rc is nonzero, else unchanged) and `select_z`, which is exactly the host cmov contract on both x86-64 and AArch64 and composes into the general form in two instructions when needed. The v1 lesson rides along: the opcode is inert until the backend recognizes diamond-shaped selects, so the qbe (and later LLVM) select-recognition work is part of the same deliverable, not a follow-up hope.

## D8: the trap model

**Status: RATIFIED as proposed. Operator, 2026-08-11.** A four-word hardware-pushed frame (pc, status, cause, aux) on a kernel-stack CSR, vectored causes, no general-purpose register saved by hardware, register saving as the kernel's calling-convention-aware choice, syscall arguments live in registers across the boundary, restartability as a spec-wide contract with defined mid-operation state for multi-step instructions, and the spec forbidding nothing about a JIT treating syscalls as call-shaped boundaries. The v1 thirteen-PUSH prologue has no v2 equivalent by construction. The reasoning below stands as the trail.

The v1 pain this kills is the thirteen-PUSH prologue repeated per handler. Proposed: on any trap the machine switches to a kernel stack named by a CSR, pushes exactly four words (pc, status, cause, aux), and vectors by cause. General-purpose registers are the kernel's problem, calling-convention-aware: the syscall path saves only what it uses, and a full register save happens only on an actual context switch. Under flagless D3 there is no condition state to preserve, so the hardware frame is genuinely minimal. Syscall arguments arrive in the D4 argument registers and stay live across the trap boundary; the return value lands in place; a single return instruction pops the frame and resumes.

Fault restartability is a spec-wide contract: a faulting instruction resumes cleanly after the kernel services the fault, and multi-step operations (the block-memory family) define their visible mid-operation register state, the maize-331/FEAT_MOPS pattern with the maize-194 restart discipline as the implementation template.

The characterization pressure is recorded here explicitly: syscall-bound code gains only 1.8x under the v1 JIT because SYS ends a compiled block. The v2 trap design should let the JIT treat a syscall as a call-shaped boundary rather than a compilation wall where feasible; the mechanism is implementation, but the spec must not forbid it.

## D12: the privileged architecture

**Status: RATIFIED as proposed. Operator, 2026-08-11.** Sv48 paging, the software TLB model, page-fault delivery, and two-level privilege carry forward with the page-table format unchanged; the CR file generalizes to a numbered CSR space with csr_read/csr_write; encoding room is reserved for a third privilege level without defining one.

Sv48 translation, the software TLB model, page-fault delivery, and supervisor/user separation are proven on v1 and quesOS's paging code ports almost verbatim if the page-table format stays identical, which is the proposal. Changes are limited to what other decisions force: the CR file generalizes to a numbered CSR space with `csr_read`/`csr_write` (making room for the D8 kernel-stack CSR, the D10 feature CSRs, and future extension state), TLB maintenance keeps its two operations under D11 naming, and encoding room is reserved for a third privilege level without defining one.

## D13: syscall-ABI continuity

**Status: RATIFIED as proposed. Operator, 2026-08-11.** The SYS instruction, its cause, the syscall number's register, and arguments-in-registers stay shaped as v1, so the quesOS port is a recompile plus a mechanical trampoline rewrite; the provider-select toggle becomes a CSR bit and the SETSYSG/CLRSYSG opcodes do not carry into v2.

The SYS instruction, its cause, the syscall number's register, and arguments-in-registers all stay shaped as in v1, so the quesOS port is the promised recompile plus a mechanical trampoline rewrite, and both ABIs (Linux-compat and native) carry over undisturbed. The one cleanup: the provider-select flag becomes a CSR bit rather than two dedicated opcodes, since dedicating opcode space to a mode toggle was always encoding extravagance, and the shrinking native provider makes the toggle progressively less load-bearing anyway.

## D4: the calling convention

**Status: RATIFIED as proposed. Operator, 2026-08-11.** Arguments r2 through r9 doubling as return registers, roughly ten callee-saved, the rest temporaries, ra at r31 and sp at r30, 16-byte stack alignment, no red zone, small structs in registers and large by reference, varargs on the stack past the register arguments. The full ABI supplement is Phase 2 prose against this register-role map.

Eight argument registers that are also the return registers (arguments r2 through r9, results in r2/r3), roughly ten callee-saved registers, the rest caller-saved temporaries, `ra` at r31, `sp` at r30, 16-byte stack alignment (headroom for a future vector extension), no red zone, small structs up to two words passed in registers and larger ones by reference, and varargs entirely on the stack past the register arguments. The full ABI supplement is Phase 2 prose; the decision fixes the register-role map and the alignment so D1's encoding examples and the backend plan can proceed.

## D9: memory architecture details

**Status: RATIFIED as proposed. Operator, 2026-08-11.** Little-endian only; bounded configurable physical memory discovered via the boot-information block plus a CSR; misaligned access defined-allow with a documented performance note; naturally aligned accesses up to eight bytes single-copy atomic; the brief's implementation notes ride as non-normative appendix material.

Little-endian only. Guest physical memory is bounded and configurable, discovered through a boot-information block the VM places in memory plus a CSR reporting its address, rather than probing. Misaligned access stays defined-allow (the v1 stance, kept deliberately as a teaching-friendly simplification with a documented performance note). Naturally aligned accesses up to eight bytes are single-copy atomic; misaligned accesses carry no atomicity guarantee. The implementation notes from the brief (flat host reservation, inline-TLB JIT loads, dirty-page tracking, the reserved capability tag plane) ride along as non-normative appendix material.

## D10: extension governance

**Status: RATIFIED as proposed. Operator, 2026-08-11.** The base freezes once, forever. Named, independently versioned extensions with per-extension opcode pages behind the D1 escape byte and their own CSR ranges; discovery via a feature-bitmap CSR plus a versioned list in the boot-information block; conformance claims name base plus exact extension set; unknown opcodes trap, never no-op.

The base freezes once, forever. Each extension has a name (`base`, `cap`, `vec`, `meter`, `atomic` as the anticipated first set), an independent version, an allocated opcode page behind the D1 escape byte, and any state it adds lives in its own CSR range. Discovery is a feature CSR (a presence bitmap for fast checks) plus an extension list in the D9 boot-information block carrying versions. A conformance claim names the base version plus the exact extension set, and the conformance suite is factored the same way, so a base-only VM is a complete, certifiable machine. Nothing in reserved space ever executes as a no-op; unknown opcodes trap, which is what makes forward compatibility testable.

## D11: the assembler conventions

**Status: RATIFIED in full. Operator, 2026-08-11.** All four convention calls are resolved: operand order source-to-destination (ruled, overruling the recommendation), always-explicit numeric bases (ruled, overruling the recommendation; the poka-yoke principle), lowercase case-sensitive mnemonics (as proposed), and the dot for length specifiers (as proposed).

Settled and restated: full lowercase word mnemonics; underscores spell compound names; the dot is reserved for operand structure, surviving inside names only as the possible length-specifier form; one canonical name per operation with the alias table deferred; the `@` memory sigil and the digit-separator flexibility carry over.

The four convention calls, as resolved:

- Operand order: **RULED, source-to-destination. Operator, 2026-08-11, overruling the destination-first recommendation.** v2 keeps v1's order: `add r1 r2 r3` reads "add r1 and r2 into r3," `store r4 @r9` reads "store r4 into memory at r9," `load @r9 r4` reads "load from memory into r4." The operator's grounds: the recommendation's premise was wrong, since not every assembler is destination-first (AT&T syntax, GAS's own dialect on x86, is source-first), and the imperative reading, verb source into destination, is the natural English parse for word mnemonics. Comma-free layout is kept, and the disassembler, the spec examples, and the toolchain all emit source-first.
- Decimal marking: **RULED, always-explicit bases are mandatory. Operator, 2026-08-11, overruling the bare-decimal recommendation.** Every numeric literal names its base: `#` decimal, `$` hex, `%` binary, with no bare numbers accepted. The operator's grounds: no accidents. A base is never inferred by the lexer or by a reader, which is the v1 discipline carried forward unchanged, and it is a correctness stance rather than a style preference. The operator named the principle poka-yoke, and it generalizes across v2: reserved opcodes trap, invalid encodings trap, memory operations cannot merge into register slices, and literals cannot be misread, all the same mistake-proofing instinct applied at the ISA, the assembler, and the tooling.
- Case: **RATIFIED as proposed.** Lowercase is canonical and the assembler is case-sensitive, keeping exactly one spelling per program element, which is the same single-source discipline as the no-alias rule.
- The length-specifier spelling: **RATIFIED as proposed.** The dot is kept (`load.zb`, `store.q`, `add.h`), because under D5 the dotted surface is small (memory operations plus `.h` arithmetic), the dot makes the width family visible as a family, and it keeps compound-name underscores unambiguous as word separation. Width letters follow the terminology ruling: byte, quarter-word, half-word, word.

## D11 addendum: immediate-move width explicitness

**Status: RATIFIED. Operator, 2026-08-11, during the Phase 2 draft review.** The bare `move` mnemonic is the register-to-register form only. Every immediate move names its width with a length specifier: the narrow forms as already drafted (`move.zb`, `move.sb`, `move.zq`, `move.sq`, `move.zh`, `move.sh`), and the 64-bit immediate form spelled `move.w`. An immediate move without a width specifier is a syntax error. Neither the base marker nor the digit count of a literal ever carries width information, so `$01`, `$0001`, and `#1` remain interchangeable spellings of the same value everywhere. The disassembler emits every immediate at its encoded width (two, four, eight, or sixteen hex digits), so a disassemble-and-reassemble round trip is byte-exact.

The grounds are the poka-yoke principle applied to encoding width: the ten-byte and three-byte spellings of "put 1 in r1" are different instructions, and the earlier draft rule (an undotted literal selects the 64-bit form) let the assembler make that choice silently. Where an encoding choice exists, the source names it; the register form stays bare because nothing is chosen there.

Alternatives considered and rejected: digit-count-as-width-declaration (rejected because it makes leading zeros semantically load-bearing, breaks base interchangeability, and orphans decimal, which has no width spelling); a value-magnitude diagnostic on the undotted form (rejected because line legality would depend on the literal's value, and a small constant wanting a wide patch slot would have no spelling); smallest-form auto-selection (rejected as a silent assembler decision, the exact instinct this ruling exists to serve).

The recorded wart: `.w` exists as a length specifier only on the immediate move, softening the bare-mnemonic-is-the-word-form rule at the single place where bareness would otherwise have to pick an encoding.

## D8 addendum: the trap-entry scratch register

**Status: RATIFIED. Operator, 2026-08-12, during the Phase 4 coherence read.** The base gains one supervisor read-write scratch CSR, named `scratch` at number $4009, with no side effects and a reset value of zero, and one instruction, `csr_swap rs $csr rd`, which atomically writes rs into the named control and status register and the register's old value into rd, under exactly the access rules and traps of a `csr_write` to the same number. The opcode is $C4, length class `op r r i2`, five bytes.

The grounds: the Phase 4 read found that a trap handler's first act, reading the trap-stack CSR, necessarily destroys one general register before anything can be saved. A syscall handler sacrifices a0, which the syscall contract rewrites with the result anyway, but an interrupt handler owes the interrupted program every register and had no register to sacrifice, so full context preservation, and with it preemptive multitasking, was unimplementable. The swap-plus-scratch pair is the RISC-V sscratch pattern: the handler exchanges a0 with a preloaded per-CPU pointer, saves everything through it including the swapped-out user a0, and re-arms the scratch register before returning. D8's frame shape and its no-general-register-saved-by-hardware rule are unchanged.

Alternatives considered and rejected: a hardware-banked register swapped on trap entry (touches D8's hardware contract directly and adds trap-entry state); dedicating tp (r1) to the kernel by convention (unsound, because the machine does not enforce the ABI and a user program that writes r1 would corrupt the kernel).

## D10 addendum: page-less extensions

**Status: RATIFIED. Operator, 2026-08-12, during the Phase 4 coherence read.** An extension allocates at most one opcode page rather than exactly one. An extension that adds no instructions, of which a purely CSR-carried extension such as the anticipated `meter` is the natural case, allocates no escape byte, and its boot-information extension-list entry carries the no-page sentinel the memory-model chapter already defines. The feature bitmap allocates one bit per ratified extension, not per allocated page. D10's original sentence describing each extension as having "an allocated opcode page" describes the common case and is not a requirement; this ruling records that reading. The grounds: the escape pages are a hard budget of seven, and spending one on an extension with no instructions wastes the scarcest resource in the architecture.

## Interactions ledger

Ratifying D3 as flagless unlocks D7's register-condition select, D8's minimal frame, and the D11 vocabulary. D2 and D1 together fix the operand byte, which D5, D6, and D7 encode against. D8 and D12 jointly define the CSR space, and D13 rides on both. D4 threads through D8 (which registers stay live across a syscall trap). D10's opcode pages depend on D1's escape structure. D11 depends on D3 for spellings and can otherwise ratify independently.
