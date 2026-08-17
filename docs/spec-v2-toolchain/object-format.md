# The Maize v2 Object Format and Linking Specification

This document is normative. It fixes the object format the Maize v2 toolchain produces and
consumes: the relocatable object, the linked executable, the archive, the relocation set, the
symbol model, and the unwind metadata a debugger or an exception runtime reads. The format is a
subset of ELF, and the subset restricts what a Maize tool emits and is obliged to accept without
ever changing what an ELF field means.

An implementer should be able to produce a byte-correct object from this document and a hex
editor. Every field below is fixed by a value or by a rule, every input has a stated outcome
including every input this subset does not admit, and nothing is left implementation-defined by
omission.

## Status

**Object format `1.0-draft`, against Maize base `2.0`. Draft; not yet fixed.**

That line is this document's header, and it carries a version of its own. This document is not
part of the Maize v2 instruction set architecture and it is versioned separately from it. A
Maize v2 conformance claim names the ISA base version and never names the version on this line,
because no conformance binary can observe an object format: a machine sees bytes in memory and
has no way to ask where they came from.

The two version lines answer different questions and move at different rates. The ISA base
freezes once and never revises, which is the promise the versioning chapter of the ISA makes and
the reason the deferral sentences in the assembly-language and calling-convention chapters
described this material as separate from that document set. This format is expected to grow
instead. Dynamic linking, debug information, thread-local storage, and position-independent code
are all named below as reserved future additions, and every one of them will move this
document's version without touching the architecture's.

A reader tracking whether they hold the current text of this document watches the version on the
line above. A reader writing a conformance claim ignores it and writes `Maize base 2.0`.

The ISA base version this document is written against appears on that line too, and it is
recorded in every conforming file, in the note section fixed below. That recording is what lets a
consumer tell which architecture a file was built for without inspecting its instructions.

## Scope and the growth clause

The Maize v2 toolchain emits ELF and accepts ELF, restricted to the constructs this document
defines. The restriction is a restriction on emission and on the obligation to consume. It is
never a reinterpretation. Every field this document uses carries its standard ELF meaning
exactly, no standard field is repurposed, and anything genuinely specific to Maize lives where
ELF reserves room for it: the processor-specific and operating-system-specific ranges for section
types, segment types, and symbol types, per-owner note namespaces, and section names carrying a
`.maize.` prefix.

Growth toward fuller ELF is therefore additive. A later consumer that accepts more of ELF never
has to reread or reinterpret a file a Maize v2 toolchain has already produced, because the
meaning of every field in that file was ELF's meaning when it was written.

The base ELF specification and the System V generic ABI are the authority for everything this
document does not restate. Where this document fixes a value, that value is binding. Where it
fixes a rule, the rule is binding. Where it says nothing, the generic ABI governs, and a
construct the generic ABI defines but this subset does not admit is handled by the consumer rule
in the section on deliberate absences.

### Rationale (non-normative)

The operator ruled on 2026-08-12 that v2 uses an ELF subset rather than a purpose-built format,
with the constraint that the subset must not wall v2 off from full ELF later. This document
implements that ruling and does not reopen it. The grounds are worth recording once, because a
reader who arrives from the v1 toolchain will ask.

Maize v1's purpose-built object format had already reimplemented ELF's data model under ELF's own
names: section kinds, attribute flags, symbol bindings and types, an undefined section index, and
a relocation named `R_MAIZE_ABS32`. It converged on ELF without collecting any of ELF's
ecosystem. Against that, LLVM's machine-code layer emits ELF natively and the project roadmap
puts LLVM ahead of GCC, DWARF conventionally lives in ELF sections, and if running Linux on Maize
remains the completeness proof then the kernel's own loader parses ELF and its userland is built
by a toolchain that emits it. The ELF path gets built eventually under any of those, so building
it once is cheaper than building a bespoke format and then ELF beside it.

Four consequences follow from the ruling and are stated below as rules rather than argued again:
ELFCLASS64 with ELFDATA2LSB, matching the machine; RELA rather than REL, because the assembler
already emits a placeholder plus a relocation and an explicit addend spares the linker from
reading the placeholder back; ET_EXEC with program headers as the loaded artifact, which retires
v1's separate executable format and makes loading a program-header walk; and the standard `ar`
archive in place of v1's bespoke container.

## Identification and the ELF header

Every file this document defines begins with a 64-byte `Elf64_Ehdr`. Every field is fixed below.
Multi-byte fields are little-endian, which is `EI_DATA`'s value saying so and the machine's own
byte order.

### The identification bytes

The sixteen bytes of `e_ident` are fixed one by one.

- `e_ident[EI_MAG0]` through `e_ident[EI_MAG3]`, offsets 0 through 3, hold `$7F`, `$45`, `$4C`,
  `$46`, which is the byte `$7F` followed by `ELF` in ASCII. A consumer whose first four bytes
  differ reports that the file is not an ELF file and stops.
- `e_ident[EI_CLASS]`, offset 4, holds 2, `ELFCLASS64`. A producer writes 2. A consumer that
  reads any other value, including 1 for `ELFCLASS32`, issues a diagnostic naming the value and
  stops, because a 32-bit class implies a different header and section-header layout that this
  subset does not define.
- `e_ident[EI_DATA]`, offset 5, holds 1, `ELFDATA2LSB`. A producer writes 1. A consumer that
  reads any other value issues a diagnostic naming the value and stops. Maize is a little-endian
  machine and this subset defines no big-endian encoding of any structure.
- `e_ident[EI_VERSION]`, offset 6, holds 1, `EV_CURRENT`. A producer writes 1. A consumer that
  reads any other value issues a diagnostic naming the value and stops.
- `e_ident[EI_OSABI]`, offset 7, holds 0, `ELFOSABI_NONE`. A producer writes 0. A consumer that
  reads any other value issues a diagnostic naming the value and stops. The value is reserved
  rather than forbidden: a future Maize operating-system ABI may claim a registered value here,
  and a consumer written against this document will then correctly refuse a file it does not
  understand instead of misreading one.
- `e_ident[EI_ABIVERSION]`, offset 8, holds 0. A producer writes 0. A consumer that reads any
  other value issues a diagnostic naming the value and stops.
- `e_ident[EI_PAD]`, offsets 9 through 15, are the seven padding bytes. A producer writes zero in
  every one of them. A consumer ignores their contents entirely and never rejects a file on
  account of them. This is the one place where a consumer accepts an unexpected value silently,
  and it is not a hole in the consumer rule below: the base ELF specification reserves these bytes
  for future use and obliges a consumer to ignore them, and a subset that rejected a nonzero
  padding byte would be reinterpreting a field whose meaning ELF has already fixed.

### The remaining header fields

The thirteen fields that follow `e_ident` are fixed one by one below, each with the value a
producer writes and the outcome a consumer produces for a value outside that rule.

- `e_type`. A relocatable object holds 1, `ET_REL`. A linked executable holds 2, `ET_EXEC`. No
  other value is emitted. A consumer that reads `ET_DYN`, `ET_CORE`, `ET_NONE`, or a value in the
  processor-specific or operating-system-specific ranges issues a diagnostic naming the type and
  stops. Those types are reserved for later revisions of this document rather than forbidden by
  it, and `ET_DYN` in particular arrives with dynamic linking.
- `e_machine`. Every file holds `$4D5A`. The section below fixes that value and states what it
  costs.
- `e_version`. Every file holds 1, `EV_CURRENT`. A consumer that reads any other value issues a
  diagnostic naming the value and stops.
- `e_entry`. An `ET_EXEC` file holds `$0000000000001000`, the architectural reset address, under
  the rule fixed in the section on the linked artifact. An `ET_REL` file holds zero, and a
  consumer of an `ET_REL` file ignores the field.
- `e_phoff`. An `ET_EXEC` file holds the file offset of the program header table, which is 64 in
  every file this document defines, because a producer places that table immediately after the
  ELF header. An `ET_REL` file holds zero. A consumer reads the field rather than assuming 64,
  since 64 is what a producer writes and not a constraint on what a consumer accepts.
- `e_shoff`. Every file holds the file offset of the section header table, which is nonzero in
  every file this document defines because every such file carries a section header table. A
  consumer that reads zero issues a diagnostic and stops.
- `e_flags`. Every file holds zero. This subset defines no processor-specific flag. A consumer
  that reads a nonzero value issues a diagnostic naming the value in hexadecimal and stops,
  rather than linking the file. The rejection is deliberate and it is the point of fixing the
  field: a later revision of this document may define a flag bit that changes how a file must be
  processed, and a tool that predates the flag has no way to honour it, so refusing is the only
  behaviour that cannot silently produce a wrong binary.
- `e_ehsize`. Every file holds 64, the size of `Elf64_Ehdr`. A consumer that reads any other
  value issues a diagnostic naming the value and stops.
- `e_phentsize`. An `ET_EXEC` file holds 56, the size of `Elf64_Phdr`. An `ET_REL` file holds
  zero, since it carries no program headers. A consumer of a file with a nonzero `e_phnum` that
  reads any value other than 56 issues a diagnostic naming the value and stops.
- `e_phnum`. An `ET_REL` file holds zero. An `ET_EXEC` file holds the number of program headers,
  which is at least two, because at least one `PT_LOAD` and exactly one `PT_NOTE` are mandatory.
  The value `$FFFF`, which base ELF uses to escape into extended numbering, is outside this
  subset, and a producer that would need more than 65534 program headers issues a diagnostic
  instead. A consumer that reads `$FFFF` issues a diagnostic and stops.
- `e_shentsize`. Every file holds 64, the size of `Elf64_Shdr`. A consumer that reads any other
  value issues a diagnostic naming the value and stops.
- `e_shnum`. Every file holds the number of entries in the section header table, counting the
  null entry at index 0, so the value is at least 1 and in practice at least 5. Extended
  numbering, in which `e_shnum` is zero and the real count lives in the null section header's
  `sh_size`, is outside this subset. A producer that would need `SHN_LORESERVE` or more sections
  issues a diagnostic instead of escaping, and a consumer that reads zero issues a diagnostic and
  stops.
- `e_shstrndx`. Every file holds the index of the section header string table, `.shstrtab`. The
  value `SHN_XINDEX`, which escapes into extended numbering, is outside this subset, and a
  consumer that reads it issues a diagnostic and stops. A consumer that reads an index of zero,
  or an index at or beyond `e_shnum`, issues a diagnostic and stops.

## The machine value and its collision risk

Every conforming file holds `$4D5A` in `e_machine`. The value spells `MZ` in ASCII, which is a
mnemonic for a reader looking at a hex dump and carries no other meaning.

**The value is unregistered.** The ELF machine registry is maintained by a third party, it
advances by a small number of allocations a year, and it has not yet reached three hundred, so
`$4D5A` sits far above anything allocated and far above anything likely to be allocated for a
long time. Maize has not applied for a value and has not been granted one. The project intends to
apply later, and until that happens the value on this line is squatted rather than owned.

**What a collision would cost.** Because the value is unregistered, a third party is free to
choose the same number for an unrelated architecture, and the registry would be within its rights
to allocate it to somebody else. A tool that identified a file by `e_machine` alone would then
accept a foreign object as a Maize object. It would not fail cleanly. It would read foreign
instructions as Maize instructions and foreign relocations against the numbering fixed below, and
produce a linked binary that is wrong in a way nothing reports.

**The mitigation, which every conforming file carries.** Identification never rests on
`e_machine` alone. Every conforming file, relocatable object and executable alike, carries a
`.note.maize.abi` note section, and a consumer checks the note together with `e_machine` before
treating a file as a Maize object. A file whose `e_machine` is `$4D5A` and whose note is absent or
malformed is diagnosed and rejected rather than linked. A foreign file that collides on
`e_machine` fails that second check, so a collision is a clean diagnostic instead of a mislinked
binary.

The note costs 28 bytes and it earns its place beyond the collision case, because it carries the
ISA base version a future extension-aware consumer will want regardless. If the registry ever
allocates a real value for Maize, changing `e_machine` is a one-number edit gated by a
discriminator that already exists in every file in the wild.

### The `.note.maize.abi` section

The section is mandatory in every conforming file. Its section header holds `sh_type` = `SHT_NOTE`
(7), `sh_flags` = `SHF_ALLOC` (2), and `sh_addralign` = 4. In an executable it is covered by a
`PT_NOTE` program header, under the rule fixed in the section on program headers, so that a
consumer reading only program headers can still find it.

The section holds exactly one note, laid out as base ELF fixes a note: three 32-bit
little-endian words, then the name, then the descriptor.

- `n_namesz` holds 6, the length of the name including its terminating zero byte.
- `n_descsz` holds 8, the length of the descriptor.
- `n_type` holds 1. Note types are scoped to the owner name rather than allocated globally, so
  this value is assigned by this document within the `Maize` namespace and collides with nothing.
- The name is the five ASCII bytes `Maize` followed by one zero byte, then two zero bytes of
  padding, which brings the name to a multiple of 4 bytes as base ELF requires.
- The descriptor is two 32-bit little-endian words. The first holds the major component of the
  ISA base version and the second holds the minor component, so a file built against Maize base
  `2.0` holds 2 and then 0. The descriptor length is already a multiple of 4 and carries no
  padding.

The erratum level of the ISA text is deliberately absent from the descriptor. The versioning
chapter fixes that no conformance claim and no machine names the erratum level, and a file that
recorded it would invite a consumer to branch on it.

A consumer that finds a `.note.maize.abi` section whose `n_namesz`, `n_descsz`, `n_type`, or name
differs from the values above issues a diagnostic naming the field and the value it found, and
stops. A consumer that reads a major version it does not implement issues a diagnostic naming
both versions and stops.

## The section model

A conforming object carries the four sections the assembly language opens, plus the auxiliary
sections a consumer needs in order to read it. This part of the document fixes each one's type,
flags, and alignment, fixes how a linker combines sections that share a name, and fixes the
naming rules that keep a Maize-specific section from colliding with a standard one.

### The four sections the assembly language opens

The assembly language opens four sections, and each maps to one ELF section with a fixed name,
type, and flag set. `SHF_ALLOC` is 2, `SHF_WRITE` is 1, and `SHF_EXECINSTR` is 4.

- `section code` becomes `.text`, with `sh_type` = `SHT_PROGBITS` (1) and `sh_flags` =
  `SHF_ALLOC | SHF_EXECINSTR`. Its `sh_size` is the number of bytes the section emitted and its
  content occupies that many bytes in the file.
- `section rodata` becomes `.rodata`, with `sh_type` = `SHT_PROGBITS` and `sh_flags` =
  `SHF_ALLOC`.
- `section data` becomes `.data`, with `sh_type` = `SHT_PROGBITS` and `sh_flags` =
  `SHF_ALLOC | SHF_WRITE`.
- `section bss` becomes `.bss`, with `sh_type` = `SHT_NOBITS` (8) and `sh_flags` =
  `SHF_ALLOC | SHF_WRITE`. Its `sh_size` is the number of bytes the section reserved and its
  `sh_offset` names a position in the file that the section does not occupy, because a
  `SHT_NOBITS` section holds no file content at all.

A producer emits a section header only for a section the module opened. A module that opens no
`bss` section carries no `.bss` header, and a consumer never depends on a section being present.

`sh_addralign` is the strongest alignment the section's content requires. In `.text` that is 1,
because the instruction encoding is byte-granular and no instruction has an alignment
requirement. In `.rodata`, `.data`, and `.bss` it is 8, because a naturally aligned access of 8
bytes or fewer is single-copy atomic on this machine and a narrower section alignment would put
that property out of a program's reach. An `align` directive naming a larger power of two raises
the section's `sh_addralign` to that value.

`sh_link`, `sh_info`, and `sh_entsize` are zero in all four of these sections.

### The auxiliary sections

A conforming relocatable object carries these in addition.

- `.symtab`, with `sh_type` = `SHT_SYMTAB` (2), `sh_flags` = 0, `sh_addralign` = 8, `sh_entsize`
  = 24, `sh_link` naming the index of `.strtab`, and `sh_info` holding the index of the first
  non-local symbol.
- `.strtab`, with `sh_type` = `SHT_STRTAB` (3), `sh_flags` = 0, `sh_addralign` = 1. It holds the
  names of the symbols in `.symtab` and begins with a zero byte, so that a `st_name` of zero
  names the empty string.
- `.shstrtab`, with `sh_type` = `SHT_STRTAB`, `sh_flags` = 0, `sh_addralign` = 1. It holds the
  section names and begins with a zero byte.
- One `.rela.<name>` section for each section that carries relocations, under the rules in the
  relocation section below.
- `.note.maize.abi`, as fixed above.

A linked executable carries `.symtab`, `.strtab`, `.shstrtab`, and `.note.maize.abi`, and carries
no `.rela.<name>` section, because every relocation has been applied. A producer may omit
`.symtab` and `.strtab` from an executable when a caller asks for a stripped output, and a
consumer of an executable therefore never requires them.

### Section naming

Three rules govern names, and together they are what keeps this subset from foreclosing full ELF.

- A section whose name and semantics ELF or the generic ABI already fixes keeps both. This
  document never gives a standard name a private meaning.
- A section carrying content specific to Maize takes a name beginning `.maize.`, or a note name in
  the conventional `.note.<owner>.<what>` shape, or a section type in the processor-specific range
  `SHT_LOPROC` through `SHT_HIPROC` or the operating-system-specific range `SHT_LOOS` through
  `SHT_HIOS`. The only such section this subset defines is `.note.maize.abi`.
- A section name this document does not define is carried through by the linker rather than
  rejected, under the concatenation rule below, so long as its type and flags are ones this
  subset admits. A section whose type falls outside `SHT_PROGBITS`, `SHT_NOBITS`, `SHT_SYMTAB`,
  `SHT_STRTAB`, `SHT_RELA`, and `SHT_NOTE` is diagnosed by name and type, and the link stops.

### Concatenation and padding

A conforming linker gathers the input sections that share a name, in the order the inputs were
presented on its command line and, within an archive, in the order the members were pulled. It
places them one after another in the output section of that name.

Each input section begins at an address that is a multiple of that input section's own
`sh_addralign`, and the output section's `sh_addralign` is the largest of its inputs'. The linker
inserts padding ahead of an input section whose start would otherwise be misaligned. The padding
it inserts is a run of `nop` instructions, each the single byte `$BF`, in a section whose flags
include `SHF_EXECINSTR`, and a run of zero bytes in every other section. Padding in an executable
section decodes rather than trapping, so a fallthrough across a boundary between two input
sections reaches the next section's first instruction instead of raising an illegal-instruction
trap.

The padding rule the linker follows is a different rule from the one the assembler follows, and
the two are easy to confuse because both are called padding. Within one input section the
assembler has already resolved every `align` directive: it emitted `nop` instructions in the code
section and zero bytes in `data` and `rodata`, and those bytes are section content that arrives
at the linker indistinguishable from any other content. The rule in this section governs only the
bytes the linker itself inserts between two input sections.

### Section indices

`SHN_UNDEF` (0) carries its standard meaning and names an undefined symbol. `SHN_ABS`
(`$FFF1`) carries its standard meaning and names a symbol whose value is absolute rather than
relative to a section.

`SHN_COMMON` (`$FFF2`) is not emitted. No producer in this toolchain emits a common symbol, so a
tentative definition arriving from C is allocated by the compiler into `.bss` rather than left for
the linker to resolve. A consumer that meets a symbol whose `st_shndx` is `SHN_COMMON` issues a
diagnostic naming the symbol and stops. The rule keeps allocation in one place, and it removes
the question of what a common symbol's alignment means when two translation units disagree.

`SHN_XINDEX` (`$FFFF`) and the extended section index table `SHT_SYMTAB_SHNDX` are outside this
subset, under the same section-count bound stated for `e_shnum`.

### Flat-mode modules produce no object

The assembly language admits a second placement model. A module that declares no section may use
the `origin` directive, and such a module assembles to a directly loadable image rather than to a
relocatable object.

A flat-mode module produces no file of this format. Its output is a raw memory image, which this
document does not describe and does not constrain, and which the boot chapter of the ISA already
covers as the artifact a machine may be handed instead of a structured executable. Nothing in
this document applies to such an image: it carries no ELF header, no sections, no symbols, and no
relocations, and it is not an input a conforming linker accepts. A module that mixes `origin` with
`section` is already a diagnostic at assembly time and never reaches this format either.

## The symbol model

`Elf64_Sym` is used unmodified, at 24 bytes per entry: `st_name` (4 bytes), `st_info` (1),
`st_other` (1), `st_shndx` (2), `st_value` (8), and `st_size` (8).

The entry at index 0 is the null symbol and holds zero in every field. A `st_name` of zero names
the empty string.

`st_info` packs the binding in its high 4 bits and the type in its low 4. The bindings this
subset uses are `STB_LOCAL` (0), `STB_GLOBAL` (1), and `STB_WEAK` (2). The types are `STT_NOTYPE`
(0), `STT_OBJECT` (1), `STT_FUNC` (2), `STT_SECTION` (3), and `STT_FILE` (4). A consumer that
meets a binding or a type outside those sets issues a diagnostic naming the symbol and the value,
and stops.

`st_other` holds `STV_DEFAULT` (0) in every symbol. Other visibilities belong with dynamic
linking and are reserved for the revision that adds it. A consumer that reads a nonzero
`st_other` issues a diagnostic naming the symbol and the value, and stops.

In a relocatable object, `st_value` of a defined symbol is the offset of the symbol from the start
of the section named by `st_shndx`. In an executable it is the symbol's virtual address. `st_size`
is the size of the object or function in bytes when the producer knows it, and zero when it does
not. The assembly language has no directive that states a size, so an object assembled from
`.mzasm` source carries zero in `st_size` for every symbol; a compiler-driven producer fills it
in.

### The ordering invariant

Within `.symtab`, every symbol whose binding is `STB_LOCAL` precedes every symbol whose binding is
`STB_GLOBAL` or `STB_WEAK`, and the section header's `sh_info` holds the index of the first
symbol that is not local. A producer that emits them out of order produces a file every ELF
consumer will misread, because the partition is what lets a consumer resolve global symbols
without scanning locals. A consumer that finds a local symbol at or after the index `sh_info`
names issues a diagnostic and stops.

### What each source construct becomes

The mapping below covers every construct of the assembly language whose object consequence is a
symbol, a relocation, or a section attribute. A construct absent from this list has none of
those, and the data directives that only emit bytes are the case worth naming: `data_byte`,
`data_quarter_word`, `data_string`, `data_string_zero`, `data_fill`, and a `data_half_word` or
`data_word` whose operands are all constant expressions contribute content to the open section
and produce neither a symbol nor a relocation.

- A label definition inside a section becomes a symbol with `st_shndx` naming that section,
  `st_value` holding the label's offset within it, `STB_LOCAL` binding, and `STT_NOTYPE` type. A
  producer that knows better may emit `STT_FUNC` or `STT_OBJECT` instead, and a consumer treats
  all three alike for the purposes of a link.
- The `global` directive changes the binding of the named symbol to `STB_GLOBAL` and moves it
  after every local symbol in the table, preserving the ordering invariant.
- The `extern` directive produces a symbol with `st_shndx` = `SHN_UNDEF`, `STB_GLOBAL` binding,
  `STT_NOTYPE` type, and zero in `st_value` and `st_size`. A module that declares a name `extern`
  and also defines it emits the definition and no undefined symbol, which is what the assembly
  language's rule that the local definition wins means in this format.
- The `constant` directive produces no symbol at all. A constant binds a value rather than an
  address, it is resolved entirely within the module that defines it, and it is not linkable. This
  is why the assembly language provides the `include` directive: a shared value crosses a module
  boundary as source text, because no symbol can carry it.
- The `reserve` directive in a `bss` section contributes to that section's `sh_size` and produces
  no symbol of its own. A label ahead of it is an ordinary defined symbol in `.bss`.
- The `align` directive contributes content, or in `bss` contributes size, and may raise the
  section's `sh_addralign`. It produces no symbol.
- A `data_half_word` or `data_word` directive carrying a relocatable expression emits a
  placeholder and one relocation, at 32 and 64 bits respectively, under the relocation rules
  below. Those two directives are the only ones that accept a relocatable expression, so a data
  site is relocated at exactly those two widths and at no other.
- One `STT_SECTION` symbol per section, with `STB_LOCAL` binding, `st_value` zero, and `st_shndx`
  naming the section, exists so that a relocation can refer to a section rather than to a named
  symbol. A producer emits these ahead of every other local symbol except the file symbol.
- An optional `STT_FILE` symbol, with `STB_LOCAL` binding and `st_shndx` = `SHN_ABS`, names the
  source file. When present it is the symbol at index 1.

`STB_WEAK` appears in the accepted set and in no producer's output from this assembly language,
because the language has no directive that declares a weak symbol. A conforming linker accepts a
weak symbol from any producer: an undefined weak symbol that no input defines resolves to zero
rather than raising an undefined-symbol diagnostic, and a weak definition loses to a global
definition of the same name without a duplicate-definition diagnostic.

### Resolution

A conforming linker resolves a symbol reference against the definitions its inputs supply. Two
global definitions of one name are a diagnostic naming the symbol and both defining files. A
global reference no input defines is a diagnostic naming the symbol and the referencing file,
unless the reference is weak. Neither case is silently resolved to zero.

## Relocations

Relocations live in `SHT_RELA` (4) sections and nowhere else. This subset defines no `SHT_REL`
section and a consumer that meets one issues a diagnostic and stops.

A section carrying relocations for a section named `<name>` is named `.rela.<name>`, so
relocations against `.text` live in `.rela.text`. Its section header holds `sh_type` = `SHT_RELA`,
`sh_flags` = 0, `sh_addralign` = 8, `sh_entsize` = 24, `sh_link` naming the index of `.symtab`,
and `sh_info` naming the index of the section being relocated.

`Elf64_Rela` is used unmodified, at 24 bytes per entry: `r_offset` (8 bytes), `r_info` (8), and
`r_addend` (8, signed). `r_info` packs the symbol table index in its high 32 bits and the
relocation type in its low 32, which is `ELF64_R_SYM` and `ELF64_R_TYPE` as base ELF defines them.

`r_offset` is the offset, from the start of the relocated section, of the first byte of the field
to be patched. In an executable it would be a virtual address, and this document defines no
relocation in an executable.

### The terms

Every computation below is written in the three terms every processor supplement uses, and this
document gives each of them the same meaning those supplements do.

- **S** is the value of the symbol named by `ELF64_R_SYM(r_info)`, after resolution. For a
  defined symbol that is its virtual address in the linked output. For an `STT_SECTION` symbol it
  is the virtual address the output section begins at.
- **A** is `r_addend`, read as a signed 64-bit value.
- **P** is the virtual address of the first byte of the field being patched, which is the address
  `r_offset` designates once the relocated section has been placed.

**P names the first byte of the field.** That is what `r_offset` already designates and it is what
P means in the x86-64, RISC-V, and AArch64 processor supplements. Maize measures a control-transfer
displacement from the instruction that follows the transfer rather than from the displacement
field, so a producer contributes an addend of -4 on a branch, a `jump`, a `call`, or a `pc_add`
target, and the four bytes of the field are exactly the distance between P and the following
instruction. One formula then serves an instruction operand and a data reference alike, and P here
means what P means everywhere else.

### The types

Four types are defined. The table gives the number, the name, the width in bytes of the field it
patches, and the computation.

| Number | Name | Width | Computation |
|-------:|:-----|------:|:------------|
| 0 | `R_MAIZE_NONE` | 0 | None. |
| 1 | `R_MAIZE_ABS64` | 8 | S + A |
| 2 | `R_MAIZE_ABS32` | 4 | S + A |
| 3 | `R_MAIZE_PCREL32` | 4 | S + A - P |

`R_MAIZE_NONE` patches nothing. It occupies an entry, it names a width of zero bytes, it computes
no value, and no fit rule can fail on it. A linker reads the entry and writes no byte. It exists
so that a producer or a tool can neutralize a relocation in place without rewriting the section,
and a producer of a fresh object has no reason to emit one.

`R_MAIZE_ABS64` writes the full 64-bit result into eight little-endian bytes. Every 64-bit value
is representable in a 64-bit field, so the fit rule cannot fail and no overflow diagnostic can
arise. The rule is stated so that the absence of a diagnostic is a consequence of the width rather
than an omission.

`R_MAIZE_ABS32` writes the low 32 bits of the result into four little-endian bytes. The result
fits when it is representable in 32 bits under a signed reading or under an unsigned reading,
which is the range -2147483648 through 4294967295 and is the same dual-reading fit rule the
assembly-language chapter states for every field in the language. The bits written are identical
under both readings. A result outside that range is a link-time diagnostic naming the symbol, the
relocated section, the offset within it, and the computed value.

`R_MAIZE_PCREL32` writes the low 32 bits of the result into four little-endian bytes. A
program-counter-relative result is a signed displacement and fits only under the signed reading,
so the range is -2147483648 through 2147483647. The unsigned half of the dual reading does not
apply here, because the field is consumed as a signed displacement by the instruction that
carries it and a value above 2147483647 would reach backward rather than forward. A result outside
that range is a link-time diagnostic naming the symbol, the relocated section, the offset within
it, and the computed value. A conforming linker does not rewrite the instruction into a longer
sequence to make the value fit, matching the assembler's own refusal to relax a branch.

### The reserved space

Type numbers 4 through 127 are reserved for future definition by this document. A consumer that
meets one issues a diagnostic naming the number and stops, because a relocation it cannot compute
is a byte it cannot patch.

Type numbers 128 through 255 are reserved for private and experimental use. A conforming linker
rejects them too, with a diagnostic naming the number, so that an experiment never reaches a
shipped binary by accident.

Type numbers above 255 are not emitted. The type field is 32 bits wide because ELF makes it so,
and this document uses only its low 8 bits.

### Where a relocation may land

A relocation patches a trailing immediate of an instruction, or a data site emitted by
`data_half_word` or `data_word`, and nothing else.

The instruction case is bounded by the instruction encoding's fixed component order, which places
an escape byte first, then the opcode byte, then the operand bytes, then the immediates. Every
relocatable operand form in the base carries its immediate as the instruction's last component:
`op r i4` for `pc_add`, `op r r i4` for the ten branches, `op i4` for `jump` and `call` with an
immediate target, and `op r i8` for `move.w`. The field a relocation patches therefore always ends
where the instruction ends, and a producer computes `r_offset` as the instruction's offset plus
its length minus the field width.

A relocation whose `r_offset` plus its field width exceeds the relocated section's `sh_size` is a
diagnostic naming the entry, and the link stops.

### Worked example: a branch target

A module contains `branch_ne r4 r0 loop_top` at offset `$20` of its `.text` section, where
`loop_top` is `extern`.

`branch_ne` is opcode `$61` with the operand form `op r r i4` and a total length of 7 bytes. The
assembler emits `$61`, then the operand byte for r4 which is `$04`, then the operand byte for r0
which is `$00`, then a four-byte placeholder of zero. The immediate therefore begins at section
offset `$23` and the instruction ends at section offset `$27`.

The relocation entry holds `r_offset` = `$23`, a symbol index naming `loop_top`, a type of 3
(`R_MAIZE_PCREL32`), and `r_addend` = -4.

Suppose the linker places this `.text` at virtual address `$1000` and resolves `loop_top` to
`$1400`. Then P = `$1000` + `$23` = `$1023`, S = `$1400`, and A = -4.

    S + A - P  =  $1400 + (-4) - $1023  =  $3D9

The value `$3D9` is 985, which fits the signed 32-bit range, so the linker writes it into the four
bytes at `$1023` in little-endian order: `D9 03 00 00`.

The arithmetic can be checked against the machine. The instruction begins at `$1020` and is 7
bytes long, so the instruction that follows begins at `$1027`. The machine adds the displacement
to that address: `$1027` + `$3D9` = `$1400`, which is `loop_top`. The addend of -4 is what carries
the difference between the address of the field and the address of the following instruction, and
it works because the immediate is the last component of the instruction.

### Worked example: a `move.w` operand

The same module contains `move.w stack_top r30` at offset `$40` of its `.text` section, where
`stack_top` is `extern`.

`move.w` is opcode `$02` with the operand form `op r i8` and a total length of 10 bytes. The
assembler emits `$02`, then the operand byte for r30 which is `$1E`, then an eight-byte
placeholder of zero. The immediate begins at section offset `$42`.

The relocation entry holds `r_offset` = `$42`, a symbol index naming `stack_top`, a type of 1
(`R_MAIZE_ABS64`), and `r_addend` = 0.

Suppose the linker resolves `stack_top` to `$0000000000090000`. Then S + A = `$90000`, and the
linker writes it into the eight bytes at virtual address `$1042` in little-endian order:
`00 00 09 00 00 00 00 00`.

No addend of -4 appears here, because an absolute relocation does not measure a distance. A
relocatable expression of the form `stack_top+#8` would instead carry an addend of 8, and the
general rule is that the addend is the constant part of the relocatable expression, less 4 when
the relocation is program-counter-relative.

## The linked artifact

A linked executable is an `ET_EXEC` file carrying program headers. The subset defines no other
loadable artifact, and v1's separate executable format has no successor in v2.

### Program headers

`Elf64_Phdr` is used unmodified, at 56 bytes: `p_type` (4 bytes), `p_flags` (4), `p_offset` (8),
`p_vaddr` (8), `p_paddr` (8), `p_filesz` (8), `p_memsz` (8), and `p_align` (8). `PF_X` is 1,
`PF_W` is 2, and `PF_R` is 4.

Two program header types are emitted. `PT_LOAD` is 1 and `PT_NOTE` is 4.

**Segments are grouped by permission.** A conforming linker emits at most three `PT_LOAD`
segments and gathers the output sections into them by the permissions their flags imply: a
segment with `PF_R | PF_X` holding `.text`, a segment with `PF_R` holding `.rodata`, and a segment
with `PF_R | PF_W` holding `.data` and then `.bss`. A linker that has no content for one of the
three omits that segment. A section whose flags do not include `SHF_ALLOC` is in no segment at
all, which is why `.symtab`, `.strtab`, and `.shstrtab` are present in the file and absent from
memory.

**Addresses.** `p_vaddr` and `p_paddr` hold the same value in every segment. Maize v2 begins
execution with paging off and every address the guest forms is a physical address until the guest
turns translation on, so a difference between the two would name a distinction the machine has no
way to honour at load time.

**Alignment.** `p_align` holds 4096 in every `PT_LOAD` segment, which is the page size Sv48 fixes
in the privileged architecture. The value is tied to that page size rather than chosen: a segment
aligned to less than a page could not be given its own permissions once the guest enables
translation, because the smallest unit a page table entry can protect is one page.

**The offset congruence.** In every `PT_LOAD` segment, `p_offset` is congruent to `p_vaddr` modulo
`p_align`. A loader that maps a file page directly to a memory page then finds the segment's bytes
at the right offset within the page.

**Uninitialised data.** `.bss` occupies no bytes in the file, so the segment that carries it holds
a `p_memsz` greater than its `p_filesz`. The bytes from `p_vaddr + p_filesz` through
`p_vaddr + p_memsz - 1` are not present in the file, and **a loader zeroes every one of them
before the first guest instruction executes**. A loader that maps the segment and leaves those
bytes holding whatever the memory held before has not loaded the file. The obligation is on the
loader because the file has no way to express the bytes without carrying them, which is the whole
purpose of `SHT_NOBITS`.

**The note segment.** Exactly one `PT_NOTE` program header is present in every conforming
executable, and it covers the `.note.maize.abi` section: `p_offset` and `p_filesz` name that
section's file extent, `p_vaddr` and `p_paddr` name its virtual address, `p_memsz` equals
`p_filesz`, `p_flags` holds `PF_R`, and `p_align` holds 4. The header is mandatory because the
note is the discriminator that makes an `e_machine` collision detectable, and a note a consumer
reading only program headers cannot find would not be a discriminator at all.

**Other segment types.** `PT_PHDR`, `PT_INTERP`, `PT_DYNAMIC`, `PT_SHLIB`, `PT_TLS`, and the
segment types in the operating-system-specific and processor-specific ranges are outside this
subset. A producer emits none of them. A consumer that meets one issues a diagnostic naming the
type and stops.

### The entry point

The architectural reset address governs, and it governs alone. Execution on a conforming Maize v2
machine begins at physical address `$0000000000001000`, that value is identical on every
conforming machine, and it depends on neither the artifact nor the loader. The machine does not
read an entry point out of a file, and nothing in this document changes that.

`e_entry` exists all the same, because a file that carried no entry point would be an ELF file
that no ELF tool could describe. This document reconciles the two by requiring that they agree.

- A conforming Maize v2 executable holds `$0000000000001000` in `e_entry`.
- A conforming Maize v2 executable includes a `PT_LOAD` segment whose address range covers
  `$0000000000001000`, so that the byte the machine executes first is a byte the file supplied. A
  linker that produced an executable with no loadable segment covering the reset address issues a
  diagnostic and emits no output.
- A consumer that reads an executable whose `e_entry` is any other value issues a diagnostic
  naming both the value it found and the reset address, and stops. It does not load the file, it
  does not load the file and begin at the reset address anyway, and it does not honour the value
  in the file.

That third rule is what leaves no reading in which the file and the machine disagree. The pair
cannot diverge silently, because a file in which they diverge is refused by every conforming
consumer, so a loader that honours `e_entry` and a machine that ignores it reach the same first
instruction in every file that loads at all. A program that wants its first instruction elsewhere
does what the boot chapter already prescribes for a structured artifact, which is to place a jump
at the reset address.

## Archives

An archive is a `System V` format `ar` file. The variant matters and is named here rather than
implied, because the `ar` dialects differ in exactly the place a reader needs certainty and a
reader who guesses wrong reads garbage rather than failing.

An archive begins with the eight-byte magic `!<arch>\n`, that is `$21 $3C $61 $72 $63 $68 $3E $0A`.

Each member follows with a 60-byte header of ASCII fields, left-justified and padded with spaces,
in this order.

| Offset | Size | Field | Content |
|-------:|-----:|:------|:--------|
| 0 | 16 | Name | The member name, terminated by `/`, or a long-name reference, or a special name. |
| 16 | 12 | Modification time | Decimal seconds since the epoch. |
| 28 | 6 | Owner id | Decimal. |
| 34 | 6 | Group id | Decimal. |
| 40 | 8 | Mode | Octal. |
| 48 | 10 | Size | Decimal, the size of the member's content in bytes, excluding the header and excluding any padding. |
| 58 | 2 | Magic | The two bytes `` ` `` and `\n`, that is `$60 $0A`. |

The member's content follows its header immediately. A member whose size is odd is followed by one
padding byte of `\n`, which is not counted in the size field, so every member header begins at an
even offset.

### Member names

A member name of fifteen characters or fewer is stored in the name field, followed by a single `/`
and padded with spaces. The trailing slash is what makes a name with trailing spaces
representable, and it is the System V convention rather than the BSD one.

A member name of sixteen characters or more is stored in the long-name member, and the name field
holds a single `/` followed by the decimal byte offset of the name within that member's content.
The name field of such a member therefore reads, for example, `/42` followed by spaces.

The BSD convention, in which a long name is stored inline in the member's content after the header
and the name field reads `#1/` followed by the name's length, is not this format. A producer never
writes it and a consumer that meets a name field beginning `#1/` issues a diagnostic naming the
member and stops, rather than reading the name's bytes as content.

### The special members

The symbol index is the first member of every archive this toolchain produces, and its name field
holds a single `/` followed by fifteen spaces. Its content is laid out as follows, and every
integer in it is **big-endian**, which is the System V convention and is deliberately not the byte
order of the machine.

- A 4-byte count of the entries that follow.
- That many 4-byte file offsets, each naming the header of the member that defines the
  corresponding symbol.
- That many zero-terminated symbol names, concatenated in the same order.

The long-name member, when present, is the second member, and its name field holds `//` followed
by fourteen spaces. Its content is the long member names concatenated, each terminated by `/`
followed by a line feed. An offset in a name field indexes into this content from its first byte.

An archive that defines no symbols still carries a symbol index member, with a count of zero and
no entries, so that a consumer never has to distinguish an archive without an index from an
archive whose index is empty.

### Determinism

A producer writes zero in the modification time, zero in the owner id, zero in the group id, and
zero in the mode of every member header. Two runs of the same tools over the same inputs then
produce archives that compare equal byte for byte, and an archive can be cached, diffed, and
checksummed. The information those fields would otherwise carry belongs to a filesystem rather
than to a build artifact.

### Member selection

A conforming linker pulls a member from an archive exactly when that member defines a symbol that
is currently undefined in the link. It does not pull a member that defines only symbols already
defined, and it does not pull every member of an archive it touches.

Pulling a member can introduce new undefined symbols, so the linker iterates within one archive
until a pass pulls nothing, and only then moves on. Archives are considered in the order they
appear on the command line. A member once pulled is never reconsidered.

Maize v1 linked whole archives instead of selecting members, so this rule is a behaviour change
rather than a restatement, and the two produce different binaries: whole-archive linking drags in
code nothing references and can turn a duplicate definition into a link failure that member
selection never sees.

## Unwind metadata

A function that uses a frame pointer is unwound by walking the saved frame pointer chain, and the
calling-convention chapter fixes that walk. A function that omits the frame pointer is unwound
from the metadata this section defines, which closes the deferral that chapter carries.

The metadata is DWARF call-frame information in an `.eh_frame` section. The section header holds
`sh_type` = `SHT_PROGBITS`, `sh_flags` = `SHF_ALLOC`, and `sh_addralign` = 8.

`.eh_frame` is the format the Linux Standard Base defines, and it is not identical to DWARF's own
`.debug_frame`. The two differ in the version byte, in whether an augmentation string is present,
in the sense of the pointer from a frame descriptor back to its common information entry, in
whether a pointer-encoding byte precedes the initial location, and in whether the section carries
a terminator. This document fixes `.eh_frame` and fixes each of those five, so that two conforming
producers agree on every byte.

DWARF is the authority for the encoding of the structures and of the call-frame instructions
themselves. This document fixes what DWARF leaves to a processor supplement, and it fixes which
constructs are inside this subset.

### The register numbering

DWARF register number **n** is the Maize register **rn**, for every n from 0 through 31. The
numbering exists nowhere else in this project and it is fixed here.

One bank suffices because floating-point values occupy the ordinary general registers on this
machine and the base defines no second register file. Numbers 32 and above are unassigned. A
consumer that meets one issues a diagnostic naming the number and stops, and a future extension
that adds a register file claims a range here rather than reusing one.

The **return-address column is 31**, which is `ra`, the link register that `call` writes.

### The factors

The **code alignment factor is 1**. Instruction addresses on this machine advance by one byte,
because the encoding is byte-granular and instruction lengths run from one byte to ten. Any
larger factor would make some advance unrepresentable.

The **data alignment factor is -8**. Saved registers are one word each, the stack grows downward,
and a factor of -8 lets a saved-register rule carry a small positive multiplier.

### The common information entry

A common information entry is laid out as follows.

- A 4-byte length, not counting the length field itself. The value `$FFFFFFFF`, which base DWARF
  uses to escape into the 64-bit length form, is outside this subset and a consumer that meets it
  issues a diagnostic and stops.
- A 4-byte identifier holding **zero**. In `.debug_frame` this field holds `$FFFFFFFF` instead,
  and the difference is how a consumer tells the two sections apart.
- A 1-byte **version holding 1**.
- An augmentation string, zero-terminated, holding exactly **`zR`**. The letter `z` says that
  augmentation data follows the return-address register, beginning with its own length, and it
  must be the first letter when it is present at all. The letter `R` says that the augmentation
  data carries one byte giving the pointer encoding used for a frame descriptor's initial location
  and range. No other augmentation letter is in this subset, so `P`, which names a personality
  routine, `L`, which names a language-specific data area, and `S`, which marks a signal frame,
  are all absent. A consumer that meets an augmentation string other than `zR` issues a diagnostic
  naming the string and stops, because a letter it does not know changes the layout of everything
  that follows it.
- The code alignment factor, as an unsigned LEB128, holding 1.
- The data alignment factor, as a signed LEB128, holding -8.
- The return-address register, as an unsigned LEB128, holding 31.
- The augmentation data length, as an unsigned LEB128, holding 1.
- The augmentation data, one byte, holding **`$00`**, which is `DW_EH_PE_absptr`. The initial
  location and the range of every frame descriptor are therefore absolute 8-byte values.
- The initial call-frame instructions, from the subset below, padded with `DW_CFA_nop` until the
  entry's total length is a multiple of 8.

### The frame descriptor entry

A frame descriptor entry is laid out as follows.

- A 4-byte length, not counting the length field itself, and never `$FFFFFFFF` or zero.
- A 4-byte pointer to the common information entry this descriptor uses, holding the **distance
  in bytes backward from the first byte of this field to the first byte of that entry's length
  field**. In `.debug_frame` the same field holds a forward offset from the start of the section
  instead, and this is the second of the five differences between the two formats.
- The initial location, 8 bytes, holding the absolute virtual address of the first instruction the
  descriptor covers. In a relocatable object this field holds a placeholder of zero and carries an
  `R_MAIZE_ABS64` relocation against a symbol in the code section.
- The address range, 8 bytes, holding the number of bytes of code the descriptor covers. This
  field is a length rather than an address and carries no relocation.
- The augmentation data length, as an unsigned LEB128, holding 0. The field is present because the
  common information entry's augmentation string begins with `z`, and it holds zero because the
  `R` letter contributes to the common entry alone.
- The call-frame instructions, from the subset below, padded with `DW_CFA_nop` until the entry's
  total length is a multiple of 8.

**The section is terminated** by a 4-byte length field holding zero, placed after the last frame
descriptor entry. A consumer reads entries until it reads a zero length or reaches the end of the
section, and a conforming producer writes the terminator rather than relying on the section's
extent. `.debug_frame` has no such terminator, and this is the last of the five differences.

### Why the pointer encoding is absolute

An `.eh_frame` section produced by most toolchains uses a program-counter-relative encoding for
the initial location, which keeps the section free of relocations. That encoding is not available
here, and the reason is a property of the ratified assembly language rather than a preference.

A frame descriptor lives in `.eh_frame` and points at code in `.text`. A program-counter-relative
pointer is the difference between the address of the pointer and the address of the code, which is
the difference of two symbols in two different sections. The assembly-language chapter admits the
difference of two symbols only when both are defined in the same section of the same module, and
it makes a difference across sections an assembly-time diagnostic rather than a relocation.
Choosing a program-counter-relative encoding would therefore require changing what a ratified
chapter says a conforming assembler does, which is a behavioural change rather than an erratum.

The absolute encoding is expressible with the relocations this document already defines, and it
costs nothing today, because this subset has no position-independent code and every executable is
linked at a fixed address. The revision that adds position-independent code reopens the question,
and it will do so from a position where a program-counter-relative encoding can be spelled.

### The call-frame instruction subset

A conforming producer emits only the call-frame instructions below, and a conforming consumer
accepts all of them. The list exists so that a consumer can be built without implementing all of
DWARF, which is what a bare reference to the DWARF standard would demand.

- `DW_CFA_nop`, which is also the padding byte.
- `DW_CFA_advance_loc`, `DW_CFA_advance_loc1`, `DW_CFA_advance_loc2`, and `DW_CFA_advance_loc4`.
- `DW_CFA_def_cfa`, `DW_CFA_def_cfa_register`, and `DW_CFA_def_cfa_offset`.
- `DW_CFA_offset` and `DW_CFA_offset_extended`.
- `DW_CFA_restore` and `DW_CFA_restore_extended`.
- `DW_CFA_same_value` and `DW_CFA_undefined`.
- `DW_CFA_remember_state` and `DW_CFA_restore_state`.

`DW_CFA_set_loc` is outside the subset, because it takes an absolute address and would need a
second relocation in the middle of an instruction stream to say what the advance instructions say
without one. The expression-valued instructions, `DW_CFA_def_cfa_expression`,
`DW_CFA_expression`, and `DW_CFA_val_expression`, are outside the subset, because accepting them
obliges a consumer to implement a DWARF expression evaluator and nothing this toolchain emits
needs one. The signed-factor variants, `DW_CFA_offset_extended_sf`, `DW_CFA_def_cfa_sf`, and
`DW_CFA_def_cfa_offset_sf`, are outside the subset, because the data alignment factor is already
negative and the unsigned forms reach every offset a Maize frame uses. Vendor extensions in the
`DW_CFA_lo_user` through `DW_CFA_hi_user` range are outside the subset.

A consumer that meets a call-frame instruction outside this list issues a diagnostic naming the
opcode byte and the entry it appeared in, and stops. Each of the exclusions above is a reservation
rather than a prohibition, and a later revision may admit any of them.

## Reserved section names for debug information

The following section names are reserved now, so that debug information can arrive later without
a change to this format.

`.debug_info`, `.debug_abbrev`, `.debug_line`, `.debug_line_str`, `.debug_str`,
`.debug_str_offsets`, `.debug_addr`, `.debug_rnglists`, `.debug_loclists`, `.debug_frame`, and
`.eh_frame`.

Three rules govern them. A producer may emit any of them and no producer is obliged to. None of
them carries `SHF_ALLOC` except `.eh_frame`, so debug information occupies file space and no
memory. A conforming linker concatenates the input sections of each name under the ordinary
concatenation rule and does not attempt to interpret their contents, which means a linker built
today handles a DWARF version that did not exist when it was built.

This document deliberately pins no DWARF version. Reserving a name commits to nothing about what
goes in it, and the version becomes a question the day debug information is emitted rather than
today.

The reservation is more than a courtesy to a future card. `.eh_frame` is already in use above, and
the register numbering this document fixes for unwinding is the same numbering debug information
will use for a variable held in a register, so the two would have had to agree in any case.

## What is deliberately absent

Every construct below is **reserved rather than forbidden**. Each is a thing this subset does not
emit today and does not oblige a consumer to accept today, and each may be added by a later
revision of this document without contradicting anything written here.

- Dynamic linking and shared objects are absent. This subset defines no `ET_DYN` file, no
  `PT_DYNAMIC` segment, no `.dynamic`, `.dynsym`, `.dynstr`, `.hash`, or `.gnu.hash` section, and
  no procedure linkage or global offset table. Every link is static and produces a complete
  executable.
- Position-independent code is absent. Every executable is linked at a fixed address, and this
  document defines no relocation type that a runtime loader would apply, which is why the unwind
  pointer encoding above can be absolute.
- Thread-local storage is absent. This subset defines no `PT_TLS` segment, no `.tdata` or `.tbss`
  section, and no thread-local relocation. The calling convention already reserves r1 as a thread
  pointer, so the mechanism has somewhere to attach and its absence here is a matter of sequencing
  rather than a design that has been ruled out.
- Section groups and COMDAT are absent. This subset defines no `SHT_GROUP` section and no
  `SHF_GROUP` flag, so a duplicate definition arriving from two translation units is a diagnostic
  rather than a deduplication.
- Symbol versioning is absent. This subset defines no `.gnu.version` section family and no
  versioned symbol names, and `st_other` holds `STV_DEFAULT` in every symbol.
- Compressed sections are absent. This subset defines no `SHF_COMPRESSED` flag and no
  `Elf64_Chdr` header, so every section's content is stored as it will be used.

### What a consumer does with a construct outside the subset

A conforming consumer that meets an ELF construct this document does not define **issues a
diagnostic naming the construct and stops**. The diagnostic names the file, and it names the
construct as specifically as the construct allows: a section by its name and type, a segment by
its type, a symbol by its name and the field that was out of range, a relocation by its type
number and its offset, a header field by its name and the value it held.

Two behaviours are ruled out and they are ruled out for the same reason. A consumer never silently
accepts a construct it does not understand, and a consumer never silently ignores one. A construct
a tool cannot honour is one whose contribution to the output is unknown, and continuing past it
produces a binary that is wrong in a way nothing reports, which is how a mislinked artifact
reaches a user.

The one exception is stated where it arises, in the rule for the `e_ident` padding bytes, and it
is an exception the base ELF specification itself imposes rather than one this document chooses.

## File naming

These are conventions rather than conformance requirements. A consumer identifies a file by its
contents, and a tool that refused a correctly formed file on account of its name would be
refusing to do its job.

- A relocatable object carries the suffix `.o`.
- An archive carries the suffix `.a`.
- A linked executable carries no mandatory suffix, which is the ELF convention. Tooling on a host
  that wants one, such as a Windows build script, chooses it in the build rather than here.

The v1 suffixes `.mzo`, `.mza`, and `.mzx` named three bespoke formats, and none of those formats
survives into v2. Keeping the names would advertise a container that is no longer there. Standard
contents take standard names, which is also what a C driver, cproc, and any future LLVM driver
expect to pass around without translating.

## Note for an implementer (non-normative)

Two facts about the existing v1 code are worth carrying into an implementation, because both are
places where a v1 habit produces a wrong byte rather than a compile error.

The v1 relocation `R_MAIZE_REL32` measures its displacement from the end of the patched field.
The `R_MAIZE_PCREL32` this document defines measures from the first byte of the field and takes
an addend of -4 from the producer, so an implementer changes the arithmetic and not only the
number. The name changed as well, because `REL32` reads as a relocation in an `SHT_REL` section
and this format has none.

The numbering here starts fresh rather than carrying v1's enumeration over, and two names survive
into it with different values: v1 gives `R_MAIZE_ABS64` the value 4 and `R_MAIZE_ABS32` the value
3, while this document gives them 1 and 2. Maize v1 is frozen rather than deleted, so both
enumerations exist in one repository at once, and a v2 tool that picked up the v1 constant would
compile, link, and emit a mispatched byte with no diagnostic anywhere. Where the v2 constants live
is a question for the implementation card rather than for this document.
