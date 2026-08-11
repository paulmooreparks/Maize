# Chapter Index

This index lists every file of the Maize v2 specification in reading order, with one line
saying what each one fixes. The front page states the draft status and the authority chain,
and the terminology chapter is the one to read first whatever else a reader skips.

## Front matter

| File | Title | What it carries |
|:-----|:------|:----------------|
| `README.md` | The Maize v2 Instruction Set Architecture | The front page: what this document set is, its draft status, the authority chain, and the reading routes. |
| `SUMMARY.md` | Chapter Index | This index. |

## Chapters

| # | File | Title | What it fixes |
|--:|:-----|:------|:--------------|
| 1 | `overview.md` | Overview | What Maize v2 is, the machine at a glance, the five design stances, and the relationship to v1. Framing rather than requirement. |
| 2 | `terminology.md` | Terminology and Conventions | The size vocabulary (a word is 64 bits), the width letters, the positional slices, the register names, the base markers, the `@` sigil, and how this document states a requirement. |
| 3 | `register-model.md` | Register Model | The thirty-two registers, the hardwired zero, the link register, the absence of a stack pointer, the fourteen positional names, and what is deliberately not a register. |
| 4 | `instruction-encoding.md` | Instruction Encoding | The six regularity invariants, the opcode byte, the escape bytes, the operand byte and its slot classes, immediates, the fifteen length classes, and the decoding sequence. |
| 5 | `memory-model.md` | Memory Model | Byte order, the flat 64-bit address space, bounded physical memory, the boot-information block, alignment, atomicity, fetch-store coherence, and why devices are not in memory. |
| 6 | `execution-model.md` | Execution Model | The instruction cycle, the program counter, atomicity and restartability, determinism, interrupt timing, and the three execution states. |
| 7 | `instruction-inventory.md` | Instruction Inventory | Every base instruction by family, one line each, with its operand form and length class, plus the list of what the base deliberately leaves out. |
| 8 | `instruction-reference-integer.md` | Instruction Reference: Constants, Integer Arithmetic, and Compares | The full entry for every move, every integer arithmetic and logic operation, the carry pair, and the ten compare predicates. |
| 9 | `instruction-reference-memory.md` | Instruction Reference: Memory, Fields, and Select | The full entry for every load and store, extract and insert, the bitfield instructions, the block-memory family, and the select pair. |
| 10 | `instruction-reference-control.md` | Instruction Reference: Control, System, and Devices | The full entry for the branches, the transfers, the system instructions, control-and-status-register access, TLB maintenance, and the two port instructions. |
| 11 | `floating-point.md` | Floating Point | The IEEE 754 contract, the rounding modes and sticky flags, comparison and NaN policy, conversion and saturation, and the full entry for all forty-four floating-point instructions. |
| 12 | `trap-model.md` | Trap Model | The cause enumeration and subcodes, the four-word frame, vectored dispatch, the no-handler and double-fault halts, return from a trap, the syscall boundary, and interrupt delivery. |
| 13 | `privileged-architecture.md` | Privileged Architecture | The two privilege levels, the control-and-status-register number layout and the complete base register list, Sv48 translation and the page-table format, and the translation cache. |
| 14 | `boot.md` | Boot | The reset address, the reset privilege and register state, what the machine has already done for the guest, what the guest owes itself, and the path to user level. |
| 15 | `abi.md` | The Calling Convention | The register-role map and ABI names, the stack and frame layout, the C type mapping, argument passing and return values, variadic calls, and the syscall register contract. |
| 16 | `device-surface.md` | Device Surface | The port space and its two instructions, bulk transfer through guest memory, unpopulated ports, device interrupts, and the seven device classes with their port contracts. |
| 17 | `assembler.md` | Assembly Language | The source form, the lexical structure and statement grammar, numeric literals and expressions, the directive set, branch targets and relocations, and the no-pseudo-instruction policy. |
| 18 | `extensions.md` | Extensions and Extension Governance | What the freeze means, what an extension is, opcode-page and register-range allocation, discovery, what an extension may and may not do, and the registry. |
| 19 | `versioning.md` | Versioning | The base version and why it does not revise, errata, extension version numbering, and exactly what a version number promises. |
| 20 | `conformance.md` | Conformance | What a conformance claim names, how the suite is factored, the test-binary discipline and its four check categories, and how reserved space is tested. |

## Appendices

| # | File | Title | What it carries |
|--:|:-----|:------|:----------------|
| A | `appendix-a-opcode-map.md` | Opcode Map | All 256 primary opcode bytes, band by band, with the mnemonic, the operand form, the length class, and the length of each, plus the enumerated reserved set. Normative. |
| B | `appendix-b-encoding-quickref.md` | Encoding Quick Reference | The length-class table, the operand-byte layout, the slot classes, the escape bytes, and the worked encodings, gathered on one page. Non-normative restatement. |
| C | `appendix-c-syscall-surface.md` | The Syscall Surface | What the machine contributes to a system call, what it deliberately does not, provider selection, and why the block-memory syscalls of v1 have no successor. |
| D | `appendix-d-glossary.md` | Glossary | Every term of art the chapters use, defined once, with the chapter that owns each one named. |
