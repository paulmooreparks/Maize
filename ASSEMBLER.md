# The Maize Assembler (mazm)

`mazm` assembles Maize assembly (`.mazm`) into either a flat memory image
(`.mzb`) that `maize` loads byte-for-byte at address 0, or, with `-c`, a
relocatable object (`.mzo`) for the `mzld` linker. This document is the complete
reference for the assembler's command line, the source syntax, and every
directive. The instruction set itself, including the opcode tables, registers,
and addressing forms, is documented in the [README](README.md), and the binary
layouts of the `.mzo` and `.mzx` formats are under "Object Files, Linking, and
Executables" there.

## Command line

    mazm [options] <input.mazm>

- `mazm file.mazm` assembles `file.mazm` into `file.mzb` beside the source.
- `-c`, `--emit-object` emits a relocatable `file.mzo` instead of a flat
  `file.mzb`.
- `--check` validates only: the full assembly pipeline runs with no filesystem
  effects. Nothing is written and nothing is removed, so a broken intermediate
  save never costs a previously good binary.
- `--stdin` reads source from standard input instead of a file, so an editor can
  validate unsaved text. It requires `--check` and `--base-path`.
- `--base-path <dir>` sets the directory `INCLUDE` paths resolve against. The
  default is the input file's directory.
- `--source-name <name>` sets the name reported in diagnostics for `--stdin`
  input. The default is `<stdin>`.
- `-h`, `--help` prints the help text.

Diagnostics are written to standard error as `mazm: <file>:<line>: error:
<message>`, one line per error, and the assembler exits nonzero. On any failed
assembly no output file is produced, and a stale output at the target path is
removed up front, so an output file that exists is always a fresh, successful
build. Unrecognized `--flags` are ignored rather than fatal, so newer editor
integrations can pass flags older assemblers do not know.

## Source structure

A `.mazm` source is a sequence of comments, block headers, instructions, and
directives. For working, tested examples see [asm/hello.mazm](asm/hello.mazm)
and the `test_*.mazm` programs under [asm/](asm/), all of which assemble and run
as part of the test suite.

### Comments

Comments run from `;` to the end of the line.

### Numeric literals

Every numeric literal carries a base prefix: `%` binary, `#` decimal, `$`
hexadecimal.

Three separator characters are accepted, in any encoding: comma (`,`),
underscore (`_`), and back-tick (`` ` ``). Separators are ignored by the
assembler: they do not affect the value, they do not count toward the
digit-count width rules below, and they may be placed anywhere between digits
and freely mixed within one literal. All three of the following emit the same
2-byte value:

```
    DATA $12,34
    DATA $12_34
    DATA $12`34
```

The examples in this document use the comma form; much of the asm/ corpus uses
the back-tick.

A literal may carry a sign (`-` or `+`) between the base prefix and the digits.
The sign does not count toward the width rules, and a negative value is
two's-complement at the selected width: `$-08` is the one-byte value $F8, as
used in `LEA $-08 BP RT` to address below the frame pointer.

A literal's written form selects its storage width, which matters both for
immediate operands and for `DATA`:

| Literal | Width | Rule |
|---------|-------|------|
| `$FF` | 1 byte | hex: up to 2 digits |
| `$FFFF` | 2 bytes | hex: up to 4 digits |
| `$DEAD,BEEF` | 4 bytes | hex: up to 8 digits (separators don't count) |
| `$0000,0000,0000,0010` | 8 bytes | hex: up to 16 digits |
| `%1010,1010` | 1 byte | binary: 8/16/32/64 digits select 1/2/4/8 bytes |
| `#300` | 2 bytes | decimal: the smallest of 1/2/4/8 bytes that holds the value |

A malformed literal (a stray character, an unparseable value) is rejected with a
diagnostic naming the token.

### Block headers

A token ending in `:` opens a block of instructions and directives:

- `name:` declares the label `name` at the current assembly address. Redeclaring
  a resolved label is an error, and the diagnostic cites both declaration sites.
- A numeric literal header such as `$0000,1000:` sets the assembly address
  (an origin marker). Execution of a flat image begins at address 0, so its
  entry block is conventionally headed `$0000,0000:`.

```
$0000,0000:             ; execution starts here
    CALL main
    HALT

main:                   ; a label at whatever address follows the block above
    CP $00 RV
    RET
```

### Label references

An instruction operand may name a label anywhere a value or address is expected.
Forward references are allowed: the slot is reserved at the reference and
patched in a fixup pass at the end of assembly. A label that is never defined is
reported per reference site, so every bad reference gets its own diagnostic.

## Directives

### INCLUDE

    INCLUDE "file.mazm"

Textually includes another source file at the point of the directive, exactly as
if its lines appeared there.

The parameter is the path of the file to include, in double quotes. A relative
path resolves against the directory of the file being assembled, or against
`--base-path` when that flag is given. Included files may themselves use
`INCLUDE`; a file that includes itself, directly or through any chain of
includes, is rejected with a diagnostic. Errors inside an included file are
reported against that file's own path and line, not the including file's.

```
INCLUDE "lib/test.mazm"
```

Placement matters when the including file uses a numeric-address block header.
Included content assembles at the current address, and a header like
`$0000,0000:` afterwards rewinds the cursor to that address, overwriting
whatever the include just emitted there. The convention is to place `INCLUDE`
after the code that must start at address 0, so the included routines append
after it:

```
$0000,0000:
    CALL run_checks
    HALT

run_checks:
    ; ... the code that must live at low addresses ...
    RET

INCLUDE "lib/test.mazm"     ; library routines append after the program
```

### LABEL

    LABEL name value

Defines a named constant. `name` becomes usable anywhere a label is accepted,
exactly as if it had been declared with `name:`, except that its value comes
from the directive rather than from the assembly address.

`name` is the identifier to define. Defining a name that already has a resolved
value is an error, and the diagnostic cites both declaration sites.

`value` is an explicit `$`/`#`/`%`-prefixed numeric literal; a bare name or a
malformed literal is rejected. Label values are 32-bit, so the value must fit in
32 bits.

```
LABEL stdout #1

    CP stdout R0            ; R0 = 1, by name
```

### DATA

    DATA value [value ...]

Emits one or more literal values at the current location, in order, each
little-endian.

Each `value` is a `$`/`#`/`%`-prefixed numeric literal, written at the width its
written form selects (see "Numeric literals" above).

```
table:
    DATA $01 $02 $03 $04    ; four 1-byte values
    DATA $1234,5678         ; one 4-byte value: 78 56 34 12 in memory
```

### STRING

    STRING "text"

Emits the bytes of a double-quoted string verbatim at the current location.

The parameter is the string to emit. Bytes are written exactly as they appear in
the source, so a UTF-8 source emits UTF-8 bytes. No terminating NUL is appended;
write `\0` explicitly where a zero terminator is needed. The backslash escapes
`\\`, `\'`, `\"`, `\0`, `\t`, `\r`, `\n`, `\a`, `\b`, `\f`, and `\v` have their
usual C meanings, and `\e` emits ESC ($1B).

```
hw_string:
    STRING "Hello, world!\n\0"
```

### ADDRESS

    ADDRESS location

Emits a 4-byte (32-bit) address constant at the current location, little-endian.

`location` is either a `$`/`#`/`%`-prefixed numeric literal or a label name. A
label may be declared later in the file (a forward reference); the slot is
reserved immediately and patched in the end-of-assembly fixup pass. A label that
is never declared is reported as undefined.

```
jump_table:
    ADDRESS handler_add     ; patched even though handler_add
    ADDRESS handler_sub     ; and handler_sub are declared below

handler_add:
    RET

handler_sub:
    RET
```

## Object mode (-c)

In object mode the assembler never resolves a symbolic operand inline: every
label reference becomes a relocation the linker fills in. Object mode uses
strict declared interfaces: a reference to a symbol the unit neither defines nor
declares `EXTERN` is an error, so a typo is caught at assembly time rather than
deferred to the linker. The one exception is a data-only external reference: a
symbol referenced solely from a data initializer (`DREF`) that the unit does not
define is auto-imported for the linker to resolve, so a pointer table may name a
symbol defined in another module without an `EXTERN` line. An undefined,
undeclared symbol used as an instruction operand is still an error.

A label reference's relocation width follows the immediate width the operand
encodes. For a two-operand data move the width is the destination sub-register
width, so `CP label Rn` materialises a full 64-bit address (`R_MAIZE_ABS64`) and
`CP label Rn.H0` a 32-bit address (`R_MAIZE_ABS32`). A label source in a store or
address comparison (`ST label @Rn`, `CMPIND`, `TSTIND`) and in the `LEA`-family
three-operand forms is a full 64-bit address (`R_MAIZE_ABS64`). Single-operand
control-transfer targets (`CALL label`, `JMP label`) use a 32-bit target.

The directives below apply only in object mode. In flat (`-c` absent) assembly
each is an inert no-op and the `.mzb` output is byte-identical, with one
refinement for `EXTERN` noted in its section.

### SECTION

    SECTION kind

Selects the section that subsequent content lands in. Until the first `SECTION`
directive, content lands in CODE.

`kind` is one of `CODE`, `RODATA`, `DATA`, or `BSS`, matched case-insensitively;
`TEXT` and the dotted forms (`.text`, `.rodata`, `.data`, `.bss`) are accepted
aliases. Any other kind is an error. Function labels declared in CODE get symbol
type FUNC; data labels declared in RODATA, DATA, or BSS get OBJECT.

```
SECTION RODATA
msg:
    STRING "Linked!\0"

SECTION CODE
main:
    RET
```

### GLOBAL and PUBLIC

    GLOBAL name
    PUBLIC name

Exports a symbol so other translation units can import it. `GLOBAL` and `PUBLIC`
are the same directive under two names: each exports with GLOBAL binding, and
both assemble to byte-identical object output. Use whichever reads better;
`PUBLIC`/`EXTERN` mirror the familiar export/import pairing.

`name` is a symbol this unit defines; its default binding without an export
directive is LOCAL. Exporting a symbol the unit never defines is an error
(`cannot export undefined symbol`).

```
GLOBAL write_string

write_string:
    ; ... visible to every unit linked with this one ...
    RET
```

### EXTERN

    EXTERN name

Declares that a symbol is defined in another translation unit.

`name` is the imported symbol. A reference to an `EXTERN`'d symbol that this
unit does not define becomes an import: an undefined symbol the linker resolves
against a matching `GLOBAL`/`PUBLIC` export. `EXTERN` emits no bytes and opens
no section, and an `EXTERN`'d name that is never referenced emits no symbol at
all. Declaring `EXTERN` for a name this unit *does* define is harmless (the
local definition wins and the declaration is a no-op), so an interface fragment
can be shared by every unit, including the one that defines the symbol.

```
EXTERN write_string

main:
    CALL write_string       ; resolved by mzld against another unit's export
    RET
```

In flat assembly, where there is no linker, a reference to an `EXTERN`'d symbol
that is never defined in the same file is reported as `unresolved external
'name'`. The `--check` mode (used by the editor's live check) accepts an
`EXTERN`'d-but-undefined reference silently, since a later link will satisfy it,
but still reports an *undeclared* undefined reference as `undefined label
'name'` so typos stay visible while editing.

### ZERO

    ZERO n

Reserves uninitialised space: advances the assembly cursor without emitting any
bytes. Only meaningful inside BSS, the section that occupies memory at load time
but no space in the object file.

`n` is a `$`/`#`/`%`-prefixed numeric literal giving the number of bytes to
reserve.

```
SECTION BSS
buffer:
    ZERO $1000              ; 4096 bytes of zero-filled scratch at load time
```

### DREF

    DREF bytes label
    DREF bytes label+offset
    DREF bytes label-offset

Embeds a relocatable pointer in data: the assembler emits placeholder zero bytes
and records a relocation, so the linker patches the slot to the label's linked
address. Use it for a pointer-in-data such as a jump table entry or the address
of another object.

`bytes` is `4` or `8` (written bare, no base prefix), selecting a 32-bit
(`R_MAIZE_ABS32`) or 64-bit (`R_MAIZE_ABS64`) reference.

`label` is the symbol the pointer refers to. It may be defined later in the same
unit, or imported from another unit via `EXTERN`.

`offset` is an optional signed addend; the linker patches the slot to the
label's address plus the addend.

```
SECTION DATA
msgs:
    DREF 8 msg_a            ; 64-bit pointer, patched by the linker
    DREF 8 msg_b+1          ; 64-bit pointer to one past msg_b
```

### ALIGN

    ALIGN n

Pads the current section with zero bytes so the next datum starts on an `n`-byte
boundary, and records `n` as the section's required alignment. The linker places
the section on a matching boundary, so the alignment holds in the linked image.

`n` is the boundary in bytes, written bare (no base prefix). It must be a power
of two (a non-power-of-two is a hard error) and at most 128.

```
SECTION DATA
filler:
    DATA $AB                ; one byte, so the ALIGN below actually pads
ALIGN 16
counter:
    DATA $0000,0000,0000,0000   ; starts 16-byte aligned, in file and at runtime
```
