# Terminology and Conventions

This chapter is normative. It fixes the vocabulary the rest of the specification uses: the
names of the operand sizes, the width letters that spell those sizes in mnemonics, the names
of the registers and their slices, the markers every numeric literal carries, the sigil that
marks a memory access, and the words this document uses when it states a requirement. A
reader who skips this chapter will misread later chapters, because the size vocabulary here
is deliberately not the one most readers arrive holding.

## The literal word

A **word** on Maize is 64 bits, which is the machine's native operand size and the width of
every general register. A **half-word** is 32 bits, a **quarter-word** is 16 bits, and a
**byte** is 8 bits. The names mean what they say: a half-word is half of a word, a
quarter-word is a quarter of one, and the arithmetic works out because the word is the
machine's own width and not some other machine's.

Two frozen conventions in wide use disagree with this one, and the disagreement is
deliberate rather than accidental. In the Intel lineage a word is 16 bits, frozen there by
the 16-bit 8086 and preserved through every widening since, so that a 64-bit x86 machine
calls 16 bits a word, 32 bits a double word, and 64 bits a quadruple word. In the RISC-V and
ARM lineage a word is 32 bits, frozen by 32-bit ancestors, so that a 64-bit machine calls
its native size a double word. Both conventions describe the width of a machine that no
longer exists, carried forward for source and documentation compatibility. Maize v2 carries
neither, because Maize has no ancestor to stay compatible with and no reason to teach a
reader that "word" means something other than the machine's word.

The consequence for a reader arriving from either lineage is small but constant. Wherever
this specification says word it means 64 bits, and it never says word for 16 or 32 bits
anywhere, in prose, in a table, in a mnemonic, or in a comment. Where a size must be
unambiguous to a reader skimming, the specification writes the bit count alongside the name.

| Name | Bits | Bytes | Width letter |
|:-----|:----:|:-----:|:------------:|
| Byte | 8 | 1 | `.b` |
| Quarter-word | 16 | 2 | `.q` |
| Half-word | 32 | 4 | `.h` |
| Word | 64 | 8 | none, except `.w` on the immediate move |

## The width letters

A mnemonic names the size it operates on with a dotted suffix, and the letters are the first
letters of the size names above: `.b` is the byte, `.q` is the quarter-word, and `.h` is the
half-word. A mnemonic with no width suffix operates on the full 64-bit word, so `load` reads
eight bytes, `add` adds two full words, and `store.q` writes two bytes. The bare form is the
word form everywhere in the instruction set, with one exception and no per-family default to
remember.

The exception is the immediate move, and it is worth stating plainly rather than hiding. A
bare `move` is the register-to-register form only, every immediate move names its width, and
the 64-bit immediate move is therefore spelled `move.w`. The `.w` letter exists nowhere else
in the instruction set. It buys the property that no immediate move can be written without
saying how wide it is, which is worth more than the tidiness of a rule with no exceptions,
since the alternative is a bare mnemonic that silently picks a ten-byte encoding for a literal
that would have fit in three.

Where a narrow operation must also say how a value reaches full width, the extension rule
sits immediately before the width letter as `z` for zero-extension or `s` for
sign-extension, giving `load.zb`, `load.sq`, `move.zh`, and their kin. The letter pair is
regular by construction: wherever one member of a `z` and `s` pair exists, so does the other.
A store needs no extension letter, because a store writes the low bytes of its source and
extends nothing, so `store.b`, `store.q`, and `store.h` are the whole family.

The dot is reserved for operand structure and for these length specifiers. It never appears
inside the name of an operation, where compound names spell their words with underscores
instead, as `multiply_high_unsigned`, `branch_lt_signed`, and `tlb_invalidate_all` do.

## Positional slices

A register slice is a named element inside a register, and the fourteen slice names are the
eight bytes, the four quarter-words, and the two half-words of a 64-bit word. They are
written as the register name, a dot, the width letter, and the element index, counting from
the least significant element as index 0.

- The bytes are `rN.b0` through `rN.b7`, where `rN.b0` holds bits 7 through 0 and `rN.b7`
  holds bits 63 through 56.
- The quarter-words are `rN.q0` through `rN.q3`, where `rN.q0` holds bits 15 through 0.
- The half-words are `rN.h0` and `rN.h1`, where `rN.h0` holds bits 31 through 0 and `rN.h1`
  holds bits 63 through 32.

Slices are positional names, not a second register file. No slice has an independent
identity, no instruction outside the extract and insert families accepts a slice, and no
memory operation ever targets one. The register-model chapter states the model and the
extract-and-insert family in the inventory states what reading and writing a slice does.

Bit numbering throughout the specification counts from 0 at the least significant bit to 63
at the most significant, and a range written "bits 15 through 0" is inclusive at both ends.

## Register names

The thirty-two general registers are named `r0` through `r31`, and those thirty-two spellings
are the canonical names. A register number in an encoding is the plain integer 0 through 31,
so the name and the number agree with no table in between.

The assembler also accepts a set of aliases. Three of them are architectural, `zero` for r0,
`ra` for r31, and `sp` for r30, and the rest are the thread pointer, frame pointer, argument,
result, temporary, and callee-saved names the calling convention assigns. Every one of those
names is a reserved word, so no program defines a symbol that shadows a register. An alias is
a second name for a register and nothing more. It does not name an operation, it
does not change an encoding, and it cannot introduce the ambiguity that mnemonic aliases
would, which is why the one-canonical-name-per-operation rule applies to mnemonics and not
to register names. The disassembler emits the calling convention's names, since a reader of
disassembly is reading code that obeys the convention.

The program counter is not a general register and has no register number. Software reads it
with `pc_add`, which writes the address of the following instruction plus a literal, and
software cannot write it except by executing a control transfer.

## Numeric literals

Every numeric literal in Maize assembly names its base with a leading marker, and there is
no default base and no bare number. The markers are `#` for decimal, `$` for hexadecimal,
and `%` for binary, so `#16`, `$10`, and `%10000` are three spellings of the same value. A
literal with no marker is a syntax error rather than a decimal number.

The requirement is a correctness stance and not a style preference. A base that is inferred
is a base that can be inferred differently by the lexer and by the person reading the line,
and the class of bug that follows is silent. Requiring the marker is the same mistake-proofing
instinct that makes a reserved opcode trap and an undefined operand form trap: the machine
and its tools refuse to guess.

A negative literal writes its minus sign after the base marker, as `#-1` or `$-20`, and the
assembler accepts two digit separators, the comma and the back-tick, between digits of a
based literal, so ``$FFFF`FFFF`` and `#1,000,000` are legal and mean what they look like. The
underscore is not a separator, because it is an identifier character. Separators carry no
meaning beyond readability, are stripped before conversion, and are not preserved anywhere in
an encoding. The assembler chapter owns the lexical detail.

Outside assembly examples, this specification writes raw bytes and encodings in hexadecimal
with the same `$` marker, so an opcode byte appears as `$A0` and a bit pattern appears as
`%101` wherever the bit-level shape is the point.

## The memory sigil

The `@` sigil marks every operand through which an instruction reaches memory, so `load @r9
r4` reads memory at the address in r9 and `move r9 r4` copies the register. The sigil is
mandatory on every memory operand and is a syntax error anywhere else. Because only the
load, store, and block-memory families touch memory at all, and because each of them carries
the sigil, a reader finds every memory access in a listing by searching for one character.

The two addressing forms are the bare form `@rN`, which uses the register's value as the
address, and the displaced form `@rN+$disp` or `@rN-$disp`, which adds a signed displacement
to it. The memory-model chapter defines what the resulting address means and the
instruction-inventory chapter defines the displacement's width.

## Assembly syntax at a glance

The syntax rules below apply to every example in this specification and to the assembler
that ships with the machine.

- Mnemonics are lowercase, and the assembler is case-sensitive, so exactly one spelling of
  each operation exists.
- Operands are separated by whitespace, never by commas.
- Operands read source to destination, so `add r1 r2 r3` adds r1 and r2 into r3, `store r4
  @r9` stores r4 into memory at r9, and `load @r9 r4` loads from memory into r4.
- A label is an identifier followed by a colon, standing alone on a line of its own, and a
  label used as a branch, jump, or call target assembles to the displacement the instruction
  requires.
- A semicolon begins a comment that runs to the end of the line.

## How this specification states requirements

The specification describes a machine in the indicative. A sentence of the form "the machine
raises the illegal-instruction trap" states what a conforming machine does, not what it
ought to do, and this document contains no requirement phrased as a recommendation. Where a
sentence describes something an implementation is free to choose, it says so in those words
and names the bound on the choice.

Four terms carry a fixed meaning wherever they appear.

- **Traps** means that the machine abandons the current instruction's remaining effects,
  delivers a trap with the named cause through the mechanism the trap-model chapter defines,
  and does so for every occurrence of the named condition. A trap is never optional, never
  advisory, and never a diagnostic the machine may skip.
- **Reserved** means that an encoding, a control-and-status-register number, a bit, or a
  field is unassigned in this version and raises a trap when used. Reserved space never
  executes as a no-operation, never reads as a defaulted value, and never behaves as an
  approximation of a neighboring encoding. A conforming machine treats reserved exactly as
  this chapter says, because that is what allows a later extension to assign the space
  without breaking a program that a machine has already accepted.
- **Defined** means that the specification names the outcome for every input, including
  inputs a conventional architecture would leave undefined. Where a chapter says an outcome
  is defined, a second implementation reaches the identical architectural state, and there is
  no case in this document where the outcome of an instruction depends on an implementation's
  choice.
- **Privileged** means that executing the instruction, or accessing the register, at user
  level raises the privileged-operation trap, and that the trap fires before any other effect
  of the instruction becomes visible.

Every behavioral statement in this specification is written so that a conformance binary can
pin it. The test is concrete: a claim belongs in a normative chapter only if a program can
be written that runs on a conforming machine, observes the claimed behavior through
architectural state, and would fail on a machine that got it wrong. Statements that fail
that test appear only where the document says they do. The overview chapter is framing
throughout, every other chapter carries such statements only in a section or a note it marks
as non-normative or as illustrative, and the encoding quick-reference appendix is a
restatement whose sources govern wherever it disagrees with them.
