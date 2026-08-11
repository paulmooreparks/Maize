# Assembly Language

This chapter is normative. It fixes the source language the Maize v2 assembler accepts: the
characters a source file may contain, the shape of a statement, the spelling of every operand
form, the directive set, the expression language, and the rules by which a symbolic branch
target becomes a displacement in the encoded instruction. The instruction inventory names the
operations and the opcode-map appendix assigns their bytes; this chapter says how a person
writes them down.

Two properties govern everything below, and both are consequences of ratified decisions rather
than matters of taste. The first is that one program element has exactly one spelling, so
mnemonics are lowercase and case-sensitive, each operation carries one canonical name, and the
assembler synthesizes no instruction that a programmer did not write. The second is
mistake-proofing: every numeric literal names its base, every memory operand carries the `@`
sigil, and every construction the assembler cannot represent exactly is a diagnostic rather
than a silent approximation. A number that could be read two ways, a value that does not fit
its field, and a slice written where the encoding has no form field for one are all errors that
stop the assembly.

## Source form

A source file is a sequence of lines of UTF-8 text. Outside string literals, character
literals, and comments, every character is a printable ASCII character, a space, or a
horizontal tab; any other byte is a diagnostic. A line ends with a line feed, and a carriage
return immediately before that line feed is discarded, so files written on either host
convention assemble identically. The last line of a file need not end with a line feed.

Leading and trailing whitespace on a line carries no meaning, and the assembler imposes no
column convention. The layout this specification uses throughout, and the one the disassembler
emits, places a label definition at column one and indents every other statement by four
spaces.

The conventional file suffix for assembly source is `.mazm`, carried over from v1. The
assembler reads whatever path it is given and attaches no meaning to the suffix.

## Lexical structure

The lexer recognizes six token classes, and whitespace separates tokens wherever two adjacent
tokens would otherwise run together.

- **Comment.** A semicolon begins a comment that runs to the end of the line. A semicolon
  inside a string literal or a character literal is an ordinary character. There is no block
  comment and no nested comment.
- **Identifier.** An identifier begins with a letter or an underscore and continues with
  letters, digits, and underscores. The dot is not in the identifier alphabet, which is what
  makes `r3.b5` structurally distinct from any label a program can name and removes the need
  for a reserved-name list covering the positional forms.
- **Numeric literal.** A numeric literal begins with one of the three base markers and is
  described below.
- **String literal.** A string literal is a run of characters between straight double quotes.
- **Character literal.** A character literal is one character between straight single quotes.
- **Punctuation.** The punctuation tokens are `:`, `@`, `.`, `+`, `-`, `*`, `/`, `&`, `|`,
  `^`, `~`, `<<`, `>>`, `(`, and `)`.

Case matters everywhere. Mnemonics, directive names, register names, and the width and format
suffixes are lowercase, and `LOAD`, `Load`, and `R5` are not the names of anything. Symbols are
case-sensitive too, so `total` and `Total` are two different symbols.

A statement occupies one line. There is no statement separator and no line-continuation
character, so a long statement stays long and a reader never has to scan backward to find where
a statement began.

## Statement grammar

The grammar below is complete for the base. Terminals that name a fixed vocabulary, such as the
canonical mnemonics, refer to the instruction inventory rather than restating it here.

    program        = { line } ;
    line           = [ statement ] [ comment ] eol ;
    statement      = label_definition | instruction | directive ;

    label_definition = identifier ":" ;
    instruction      = mnemonic { operand } ;
    directive        = directive_name { operand } ;

    mnemonic       = ? a canonical mnemonic from the instruction inventory ? ;
    directive_name = "section" | "origin" | "align"
                   | "data_byte" | "data_quarter_word" | "data_half_word" | "data_word"
                   | "data_string" | "data_string_zero" | "data_fill"
                   | "reserve" | "constant" | "global" | "extern" | "include" ;

    operand        = register | slice | memory | expression
                   | string_literal | section_kind ;

    register       = "r" register_number | register_alias ;
    register_number= "0" | "1" ... | "31" ;
    register_alias = "zero" | "ra" | "sp" | ? an ABI alias from the calling-convention chapter ? ;

    slice          = register "." ( "b" byte_index | "q" quarter_index | "h" half_index ) ;
    byte_index     = "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" ;
    quarter_index  = "0" | "1" | "2" | "3" ;
    half_index     = "0" | "1" ;

    memory         = "@" register [ ( "+" | "-" ) expression ] ;

    section_kind   = "code" | "rodata" | "data" | "bss" ;

An empty line and a line holding only a comment are both legal and emit nothing. A label
definition stands alone on its line and never shares a line with an instruction, which keeps
one production per line and keeps the instruction column from shifting when a label is added or
removed.

The `operand` production offers `register` and `expression` as alternatives, and the derivation
is unambiguous all the same, because every register name is a reserved word that no symbol may
take. A token that spells a register name derives `register`, and every other identifier
derives `expression`. That rule is load-bearing for `jump`, `call`, and `sys`, each of which
has a register form and an immediate form of a different length, so a program carrying a label
named `a0` would otherwise have two readings of `jump a0` that emit different bytes and shift
every address after them.

The sign in the displaced memory form governs the whole expression that follows it, because
the `expression` production is greedy and the sign sits outside it. The operand
`@sp-slot_a-#4` therefore names the address in r30 minus the value of `slot_a-#4`, not the
address in r30 plus the value of `-slot_a-#4`. A program that means the second grouping writes
it out as `@sp+(#0-slot_a-#4)`.

## Registers, aliases, and slices

A register operand names one of the thirty-two registers as `r0` through `r31`. The number
carries no leading zero, so `r5` is the register and `r05` is a diagnostic, which keeps a
register to a single spelling in source, in listings, and in a text diff.

Three aliases are architectural and always available. They are `zero` for r0, `ra` for r31, and
`sp` for r30. The calling-convention chapter names the rest against the same register numbers,
and the assembler accepts every one of them anywhere a register operand appears. Those names
are the thread pointer `tp` for r1, the argument and result names `a0` through `a7` for r2
through r9, the temporaries `t0` through `t9` for r10 through r19, the saved registers `s0`
through `s8` for r20 through r28, and the frame pointer `fp` for r29. An alias is a register
name and not a mnemonic, so accepting several
spellings for one register does not weaken the one-canonical-name rule for operations. The
disassembler emits the ABI names.

A sliced operand names one element of a register with a dot, a width letter, and an index:
`r3.b5` is byte 5, `r3.q2` is quarter-word 2, and `r3.h1` is half-word 1. The index is a literal
digit and never an expression, because the index travels in the operand byte's form field and
the encoding admits no other value there. A slice is legal only in an operand slot the
instruction declares sliced, which in the base means the source of an `extract` and the
destination of an `insert`, and a slice written anywhere else is a diagnostic. The complementary
rule holds as well: an `extract` source and an `insert` destination are always written sliced,
so `extract.zb r3 r7` is a diagnostic rather than a shorthand for byte 0.

## Numeric literals

Every numeric literal names its base with a marker, and a token that begins with a digit is a
syntax error. This is the poka-yoke stance stated plainly: a base is never inferred by the
lexer, and it is never inferred by a reader either. The cost is one character per literal, and
the benefit is that `10` cannot quietly be sixteen on one line and ten on the next.

- `#` marks a **decimal** literal, so `#100` is one hundred.
- `$` marks a **hexadecimal** literal, so `$100` is two hundred fifty-six. The digits `a`
  through `f` may be written in either case, and the disassembler emits uppercase.
- `%` marks a **binary** literal, so `%100` is four.

A sign follows the marker rather than preceding it, so a negative literal is written `$-1` or
`#-24`. A leading `+` is accepted and means what its absence means.

Two characters group digits, the comma and the back-tick, which are exactly the two the v1
assembler accepted and v2 carries them over unchanged. Either is legal between two digits of
a based literal, carries no value, is stripped before the digits are converted, and may
appear as often as reads well, so ``$FEDC`BA98`7654`3210``, ``%0100`0001``, and `#1,000,000`
are all ordinary literals. A separator immediately after the base marker, after the sign, or
at the end of the literal is a diagnostic. The underscore is not a digit separator, because
the underscore is an identifier character.

A character literal is a single character between straight single quotes and stands for that
character's code point value, so `'A'` is 65. A character literal is not a bare number and does
not violate the mandatory-base rule, because it names its value by identity rather than by
digits in an unstated base. A code point above 255 is a diagnostic.

Both character literals and string literals accept eight escapes and no others: `\\`, `\"`,
`\'`, `\n`, `\r`, `\t`, `\0`, and `\xHH` for exactly two hexadecimal digits. An unrecognized
escape is a diagnostic rather than the escaped character.

## Expressions

An expression computes a value from literals, symbols, and operators, and it may appear
wherever this chapter's grammar admits one: an immediate operand, a memory displacement, a
branch target, and a directive operand. Expression arithmetic is 64-bit two's complement and
wraps on overflow.

An expression contains no whitespace. Whitespace ends an operand, so `add r1 $10+#2 r3` passes
three operands and `add r1 $10 + #2 r3` passes five and is a diagnostic. Requiring expressions
to be written solid is what lets operands stay comma-free without any ambiguity about where one
operand ends and the next begins.

The operators, in order of increasing precedence, are these:

    |            bitwise or
    ^            bitwise exclusive or
    &            bitwise and
    << >>        left shift, arithmetic right shift
    + -          addition, subtraction
    * /          multiplication, truncating signed division
    - ~          unary negation, unary bitwise complement
    ( )          grouping

There is no remainder operator. The `%` character is the binary base marker, and giving it a
second meaning inside an expression would make `#7%#3` and `%11` two constructions a reader has
to disambiguate by context, which is exactly what the base markers exist to prevent. A program
that needs a remainder of two assembly-time constants computes it with division and
multiplication. Division by zero in an expression is a diagnostic.

An expression whose leaves are all literals or all defined constants folds to a value at
assembly time. An expression that names a symbol whose address the linker fixes is
**relocatable**, and only four relocatable forms exist:

- A symbol alone.
- A symbol plus a constant expression.
- A symbol minus a constant expression.
- One symbol minus another symbol defined in the same section of the same module, which folds
  to a constant.

Every other appearance of an unresolved symbol is a diagnostic. Multiplying a symbol, masking
one, or subtracting two symbols across sections all fail at assembly time rather than producing
a relocation no object format can express.

An expression's value must fit the field it lands in, and one rule decides fit for every field
in the language. A field of N bits accepts any literal whose value is representable in N bits
under a signed reading or under an unsigned reading, so the acceptance range of an N-bit field
runs from -2^(N-1) through 2^N - 1. The bits emitted are the low N bits of the value's
two's-complement representation, and those bits are identical under both readings, so the
dual acceptance range never leaves the encoded bytes in doubt. A value outside the acceptance
range is a diagnostic, and the assembler never truncates a literal to make it fit.

Every immediate field in the base appears below with its width and the range the rule gives it.

- The memory displacement is 16 bits and accepts -32768 through 65535.
- The ALU immediate is 32 bits and accepts -2147483648 through 4294967295.
- The displacement of a branch, a `jump`, a `call`, and a `pc_add` is 32 bits and accepts the
  same range as the ALU immediate.
- The shift-count immediate is 8 bits and accepts -128 through 255.
- The syscall number of `sys #imm` is 8 bits and accepts -128 through 255.
- The control-and-status-register number of `csr_read`, `csr_write`, and `csr_swap` is 16 bits
  and accepts -32768 through 65535.
- The bit position and the bit width of the general bitfield instructions are 8 bits each and
  each accepts -128 through 255. Whether a value that fits is also a valid operand is the
  instruction's own rule, stated in its entry, and the assembler applies that rule after the
  fit test rather than in place of it.
- The immediate-move field is 8 bits for `move.zb` and `move.sb`, 16 bits for `move.zq` and
  `move.sq`, 32 bits for `move.zh` and `move.sh`, and 64 bits for `move.w`. The `z` and `s`
  letters name how the field is extended into the destination register, not what the field
  accepts, so the two spellings at one width accept the same range. A 64-bit field accepts
  every value an expression can compute, since expression arithmetic is 64-bit.
- A `data_byte`, `data_quarter_word`, `data_half_word`, or `data_word` operand accepts the
  range of the width it emits, so a `data_byte` operand accepts -128 through 255 and a
  `data_quarter_word` operand accepts -32768 through 65535.

Three of this specification's own examples turn on the dual reading. `move.sb $FF r3`
assembles because 255 is within the unsigned reading of an 8-bit field, and the byte emitted
is `$FF`, which the instruction sign-extends to -1. `move.sq $8000 r4` assembles the same way
at 16 bits and `move.sh $FFFFFFF8 r5` at 32 bits, and in each case the register ends up
holding the negative value the instruction's entry states.

## Symbols

A label definition is an identifier followed by a colon on a line of its own, and it binds that
identifier to the address of the next byte the current section emits. A label may be referenced
before it is defined, because no instruction's length depends on the value of an expression:
every length is fixed by the mnemonic and the operand syntax alone, so the assembler assigns
every address on its first pass over the file and never revises one afterward.

A constant is defined by the `constant` directive and binds an identifier to a value rather
than to an address. A constant is defined before it is used, and a use ahead of the definition
is a diagnostic; the restriction costs nothing in practice and it makes a cyclic definition
impossible to write.

A symbol defined in a module is local to that module unless a `global` directive exports it. A
symbol a module uses but does not define is declared with `extern`, and using an undeclared,
undefined symbol is a diagnostic rather than a hopeful reference the linker sorts out. One
identifier is reserved: `here` names the address of the first byte of the statement in which it
appears, and a program may not define a symbol with that name.

Every register name is a reserved word as well. The thirty-two canonical names `r0` through
`r31`, the three architectural aliases `zero`, `ra`, and `sp`, and every calling-convention
alias the registers section lists are reserved, and a label definition or a `constant`
definition that names one is a diagnostic. Reserving them costs a program nothing, since no
program needs a symbol called `sp`, and it settles the one place where the grammar would
otherwise admit two derivations of the same token. A token that spells a register name is a
register, and it is never a symbol.

## Directives

Directives are ordinary lowercase words in the same alphabet as mnemonics, and they carry no
sigil. The dot is reserved for length and format specifiers and for positional slices, so no
directive begins with one. Every directive name is a reserved word and cannot be used as a
symbol.

### Placement: sections and origin

The `section` directive opens one of four sections and closes whichever section was open:
`section code`, `section rodata`, `section data`, and `section bss`. A module that declares a
section is assembled to a relocatable object, and the linker places each section. Instructions
are legal in the code section only. The `bss` section holds no emitted bytes, so `reserve` and
`align` are the only directives legal inside it.

The `origin` directive takes a constant expression and sets the address at which subsequent
bytes are assembled. It is legal only in a module that declares no section, which is the flat
mode that produces a directly loadable image rather than an object file. Mixing `origin` with
`section` in one module is a diagnostic, because the two placement models answer the same
question and a module that uses both leaves the answer to whichever came last.

The `align` directive takes a constant expression that is a power of two and advances the
current address to the next multiple of it, emitting padding if the address is not already
aligned. Padding in the code section is a run of `nop` instructions, which is exact because
`nop` is one byte. Padding in the `data` and `rodata` sections is a run of bytes whose value is
zero. In the `bss` section `align` emits nothing and only advances the current address, which
is what keeps the section true to the rule that it holds no emitted bytes. An alignment that
is not a power of two, or that is zero, is a diagnostic.

### Data emission

Four directives emit an integer at each of the machine's four widths, and each takes one or
more expressions and emits them in order, little-endian, with no padding between them.

- `data_byte` emits one byte per operand.
- `data_quarter_word` emits one quarter-word, sixteen bits, per operand.
- `data_half_word` emits one half-word, thirty-two bits, per operand.
- `data_word` emits one word, sixty-four bits, per operand.

A relocatable expression is accepted by `data_half_word` and `data_word` and emits a placeholder
plus an absolute relocation of that width. The narrower two directives take constant
expressions only, since no relocation is defined at those widths. This is how v2 spells what v1
spelled with a separate reference-emitting directive: the width directive already knows the
width, so the reference needs no directive of its own.

Two directives emit text. `data_string` emits the bytes of a string literal with no terminator,
and `data_string_zero` emits them followed by one zero byte. Each takes exactly one string
literal.

The `data_fill` directive takes a count expression and a value expression and emits that many
copies of the value, one byte per copy. The value operand accepts the byte range, -128 through
255, and a count of zero emits nothing and is not an error.

The `reserve` directive takes a constant expression and advances the current address by that
many bytes without emitting any. It is the way a `bss` section names storage, and using it in a
section that carries bytes is a diagnostic.

### Symbols and linkage

The `constant` directive takes an identifier and a constant expression and binds the one to the
other for the rest of the module. Redefining a constant is a diagnostic; a program that wants a
second value gives it a second name.

The `global` directive takes an identifier the module defines and exports it. The `extern`
directive takes an identifier the module uses and does not define, and declares that the linker
will supply it. Declaring a name `extern` that the module also defines is harmless and the local
definition wins, which keeps a shared declarations file usable from the module that implements
what it declares.

### Inclusion

The `include` directive takes a string literal naming a path and assembles that file's text at
the point of the directive, as though its lines had been written there. A relative path resolves
against the directory of the file containing the directive, never against the working directory,
so a source tree moves without breaking. A cycle among included files is a diagnostic naming the
cycle, and the diagnostic for an error inside an included file names the included file and its
line, then the including file and its line.

Inclusion earns its place because a constant is not a linkable symbol. Syscall numbers, device
port numbers, and control-and-status-register numbers are values rather than addresses, so
`extern` cannot carry them between modules and a shared declarations file is the only mechanism
that can. The toolchain ships such a file, defining a constant for each control and status
register in the privileged-architecture chapter's table under that table's own names, and the
examples elsewhere in this specification that write `csr_read fcsr r5` or
`csr_write r1 trap_vector_base` assume it is included.

## Branch targets, pc_add, and relocations

Branches, jumps, calls, and `pc_add` all encode a signed 32-bit displacement measured from the
address of the instruction that follows them. Source is written in addresses rather than in
displacements, and the assembler does the subtraction.

The rule that decides between the two readings is lexical, and the mandatory base markers are
what make it safe. A symbol or a relocatable expression in a target slot names an address, and
the assembler emits the displacement that reaches it. A numeric literal in the same slot is the
displacement itself and is emitted unchanged, which is how a program written against a fixed
layout, or a test that means to probe a particular encoding, says so. Neither form can be
mistaken for the other, because a symbol never begins with a base marker and a literal always
does.

    branch_ne r4 r0 loop_top      ; the assembler computes the displacement
    branch_ne r4 r0 $-18          ; the displacement is written out, unconverted

The same rule covers `pc_add`, which is how position-independent code names an address:
`pc_add message r10` puts the address of `message` in r10 regardless of where the image is
loaded, and `pc_add #0 r10` puts the address of the following instruction there.

A target the module defines in the same section folds at assembly time, and the assembler checks
that the displacement fits in a signed 32-bit field. A target that is `extern`, or that lives in
another section, emits a placeholder of zero and a 32-bit program-counter-relative relocation
that the linker resolves, and the relocation's numbering and name are deferred to a future
object-format and linking specification, separate from this document set, which leaves the
assembler's own behavior fully defined because the bytes it emits are the ones the
instruction inventory fixes. A displacement that does not fit is a diagnostic, and the assembler does not rewrite the
branch into a longer sequence, because rewriting one instruction into several is exactly what
the next section forbids.

Exactly one instruction immediate outside the target slots accepts a relocatable
expression, and it is the immediate of `move.w`, which emits an eight-byte placeholder and
a 64-bit absolute relocation the linker resolves, under the same deferred numbering as the
program-counter-relative form. That is what lets startup code write `move.w stack_top r30`
before any section layout is known. Every other instruction immediate takes a constant
expression only, and a relocatable expression in one is a diagnostic, matching the rule the
data directives state, because absolute relocations exist at 32 and 64 bits and nowhere
narrower.

## The pseudo-instruction policy

The assembler synthesizes nothing. Every mnemonic in a source file, and every mnemonic in a
listing or a disassembly, is a real operation from the instruction inventory, and every source
line that is an instruction assembles to exactly one instruction. A reader counting instructions
in a listing counts the instructions the machine executes.

Four things this rules out are worth naming, because every one of them is a service some other
assembler provides:

- No constant materialization. A value too wide for the form the source names is a diagnostic,
  not a silently emitted pair of instructions and not a truncation to the named width.
- No branch relaxation and no long-branch expansion. A displacement that does not fit is a
  diagnostic.
- No convenience aliases that expand. Clearing a register is `move r0 r5`, which is the real
  `move` instruction, and a program that wants a nop writes `nop`, which is the real one-byte
  instruction.
- No stack-frame, register-save, or calling-convention services. The prologue and epilogue in
  the worked example below are instructions the programmer wrote.

The one sanctioned convenience is **mnemonic selection**, in which one canonical name covers
several encodings of the same operation and the operand syntax picks which encoding is emitted.
Selection never changes the operation, never changes the number of instructions, and never
changes what the source says; it only spares the programmer from spelling a name per encoding.
The move family shows where selection stops:

- `move rs rd` is the register-to-register form and nothing else. The bare mnemonic never takes
  a literal, so no immediate encoding is ever selected by inference.
- `move.w $imm rd` is the 64-bit immediate form, and `move.zb $imm rd` with its five siblings
  are the narrow immediate forms. Every immediate move names its width in the mnemonic, an
  immediate move written without a width specifier is a diagnostic, and neither the base marker
  nor the digit count of a literal carries width information.
- `load @rb rd` selects the bare form and `load @rb+$20 rd` selects the displaced form. Writing
  a displacement of zero, as in `load @rb+#0 rd`, selects the displaced form deliberately and
  emits its longer encoding.
- `add rs1 rs2 rd` selects the register form and `add rs1 $imm rd` the immediate form, and the
  same holds for every arithmetic, logical, and compare operation that has both.
- `jump target` and `call target` select the displacement forms, while `jump rs` and `call rs`
  select the register forms. `sys #imm` and `sys rs` divide the same way.

Two properties of the emitted bytes follow from all of this and are directly testable. The
assembler never emits a reserved opcode byte, and the assembler never emits an operand byte
whose form field is undefined for its slot class, because a slice in a plain slot is rejected in
the front end. Every byte string the assembler produces decodes cleanly under the rules of the
instruction-encoding chapter.

## Diagnostics

Every violation this chapter names is a diagnostic that reports the file, the line, and the
offending token, and an assembly that produces one diagnostic produces no output file. The
assembler continues past a diagnostic in order to report further ones in the same run, so a
single invocation finds more than the first mistake, and its exit status is nonzero whenever it
reported any.

There is no warning class that a program can accumulate. Every condition is either accepted or
rejected, which is the same stance the machine takes toward an encoding it does not define.

## Round-tripping with the disassembler

The disassembler emits this language. Its output reassembles to the byte string it was given,
byte for byte, for any byte string the assembler produced, and that property is a test rather
than an aspiration. Where the disassembler must choose a spelling it chooses the canonical one:
lowercase mnemonics, uppercase hexadecimal digits, ABI register aliases, symbolic targets where
a symbol table supplies a name and hexadecimal displacements where none does.

Every immediate is printed at its encoded width, so a one-byte immediate prints as two
hexadecimal digits, a two-byte immediate as four, a four-byte immediate as eight, and the
eight-byte immediate of `move.w` as sixteen. Leading zeros are kept rather than trimmed, which
shows a reader the size of the field the bytes occupy. The reassembler still takes the width
from the mnemonic alone, since the digit count is presentation and never a selector.

The round trip holds for a negative immediate, and the field-fit rule is what makes it hold.
The disassembler prints a field's bits as an unsigned hexadecimal number at the encoded width,
the fit rule accepts that number under its unsigned reading, and the bits the assembler then
emits are the low bits of the same value, which are the bits it was given. A branch
displacement of -24 prints as `$FFFFFFE8` and reassembles to the same four bytes, and a memory
displacement of -8 prints as `$FFF8` and reassembles to the same two. That is what makes the
round trip this section opens with total rather than limited to the immediates that happen to
be non-negative.

## A worked example

The routine below formats a 64-bit value as sixteen lowercase hexadecimal characters. It reads
a table from read-only memory, loops over the eight bytes of the value, calls a helper for each
one, extracts a byte slice, inserts into another, and stores at two widths, so it exercises the
whole surface this chapter defines.

The example keeps its loop state in the stack frame and reloads it after the call. That is
deliberate: the calling-convention chapter names which registers survive a call, and a routine
that keeps nothing live in a register across one depends on none of that.

    ; ==================================================================
    ; word_to_hex: format a word as sixteen lowercase hexadecimal digits.
    ;
    ;   r2   the value to format
    ;   r3   the destination buffer, at least sixteen bytes
    ; returns
    ;   r2   the address one past the last character written
    ; ==================================================================

    constant frame_size    #32          ; a multiple of 16, per the ABI
    constant slot_value    #0           ; the value, shifted as it is consumed
    constant slot_cursor   #8           ; where the next character goes
    constant slot_count    #16          ; bytes of the value still to format
    constant slot_link     #24          ; the caller's return address

    section rodata

    hex_digits:
        data_string "0123456789abcdef"

    section code

    global word_to_hex

    word_to_hex:
        subtract sp frame_size sp       ; open the frame; there is no red zone
        store ra @sp+slot_link          ; the call below overwrites r31
        store r2 @sp+slot_value
        store r3 @sp+slot_cursor
        move.zb #8 r4
        store r4 @sp+slot_count

    format_loop:
        load @sp+slot_count r4
        branch_eq r4 r0 format_done     ; r0 supplies the zero to test against

        load @sp+slot_value r5
        extract.zb r5.b7 r2             ; the top byte becomes the first argument
        load @sp+slot_cursor r3         ; the cursor becomes the second
        call byte_to_hex

        load @sp+slot_value r5
        shift_left r5 #8 r5             ; bring the next byte up to byte 7
        store r5 @sp+slot_value

        load @sp+slot_cursor r6
        add r6 $2 r6                    ; two characters were written
        store r6 @sp+slot_cursor

        load @sp+slot_count r4
        subtract r4 $1 r4
        store r4 @sp+slot_count
        jump format_loop

    format_done:
        load @sp+slot_cursor r2         ; the end of the text is the result
        load @sp+slot_link ra
        add sp frame_size sp
        return

    ; ==================================================================
    ; byte_to_hex: write the two characters of one byte.
    ;
    ;   r2   the byte to format, in its low eight bits
    ;   r3   where to write the two characters
    ;
    ; The routine calls nothing, so it needs no frame and leaves the link
    ; register alone. It uses r10 through r12 as temporaries.
    ; ==================================================================

    byte_to_hex:
        pc_add hex_digits r10           ; position-independent table address

        shift_right_logical r2 #4 r11   ; the high nibble
        and r11 $F r11
        add r10 r11 r11
        load.zb @r11 r11                ; its character

        and r2 $F r12                   ; the low nibble
        add r10 r12 r12
        load.zb @r12 r12                ; its character

        insert.b r12 r11.b1             ; pack both into one quarter-word
        store.q r11 @r3                 ; little-endian, so the high digit
        return                          ; lands first, which is what hex wants

Four lines of that listing are worth reading twice. The `store ra @sp+slot_link` line uses a
displacement written as a constant expression, which folds at assembly time exactly as a literal
would. The `extract.zb r5.b7 r2` line carries a slice in the only slot that admits one, and its
index travels in the operand byte's form field rather than in an immediate. The
`insert.b r12 r11.b1` line is the one merge site in the whole routine, since every other write
to a register replaces the whole word. The `pc_add hex_digits r10` line crosses a section
boundary, so it emits a placeholder and a program-counter-relative relocation that the linker
resolves, and the routine works wherever the image is loaded.
