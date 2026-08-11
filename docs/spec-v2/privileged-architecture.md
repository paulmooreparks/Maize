# Privileged Architecture

This chapter is normative. It fixes the privilege levels and what separates them, the
numbering and contents of the whole control-and-status-register space, the Sv48 address
translation the machine performs, the page-table format, the delivery of page faults, and the
architectural meaning of the two translation-cache maintenance instructions. The trap-model
chapter owns the cause numbers and the frame; this chapter owns the registers that chapter
names and the translation that produces its page faults.

The privileged machine is deliberately small. A kernel needs a privilege boundary, a way to
name and reach machine state, an address translation, and a way to keep the translation cache
honest, and that is the entire content of this chapter.

## Privilege levels

Maize v2 defines two privilege levels and reserves encoding room for a third.

- **User**, encoding `%00`, is where application code runs. User code reaches the ordinary
  instruction set, the user-level control and status registers, and the pages whose
  page-table entries mark them user-accessible.
- **Supervisor**, encoding `%01`, is where a kernel runs. Supervisor code reaches everything.

Encodings `%10` and `%11` of the privilege field are reserved. No base instruction produces
them, no trap enters them, and every place the field appears rejects them: a write of a
reserved encoding to the status register raises the illegal-operand trap, and a
control-and-status-register number naming a reserved level raises the illegal-operand trap on
any access. Reserving the encodings costs nothing today and means a hypervisor level can be
added later without moving a single field.

The current level lives in the privilege field of the status register, which the trap-model
chapter lays out. The machine sets it to supervisor at reset and on every trap entry, and
`trap_return` sets it from the status word on the frame. There is no instruction that raises
privilege, so user code reaches supervisor only by trapping into a handler the kernel
installed.

### What user mode cannot do

Four boundaries separate user from supervisor, and each one has a named outcome when user code
crosses it.

- Seven instructions are privileged, and executing any of them at user level raises the
  privileged-operation fault. They are `trap_return`, `halt`, `wait_for_interrupt`,
  `tlb_invalidate_all`, `tlb_invalidate_address`, `port_in`, and `port_out`.
- A control and status register whose number names supervisor is unreachable from user level,
  and an access to one raises the privileged-operation fault. The number itself carries the
  requirement, so the check needs no lookup.
- A page whose page-table entry has the user bit clear is unreachable from user level, and an
  access to one raises the page fault its access kind names.
- Every device is behind the port instructions, which are privileged, so no user-mode
  instruction reaches a device register. There is no memory-mapped device state to reach by
  another route.

Everything else is available at both levels, and the list of privileged instructions is
closed. In particular `sys` is not privileged, because it is the intended way for user code
to enter the kernel, and `breakpoint` is not privileged, because a debugger has to be able to
plant one in user code.

## The control and status register space

Control and status registers hold every piece of architectural state that is not a general
register. Three instructions reach the whole space, `csr_read $csr rd`, `csr_write rs $csr`,
and the atomic exchange `csr_swap rs $csr rd`, and none of the three is itself privileged;
the register number carries the access rules, and a `csr_swap` is checked exactly as a
`csr_write` to the same number.

### The number layout

A register number is an unsigned 16-bit value with three fields.

    bits 15:14   privilege   %00 user, %01 supervisor, %10 and %11 reserved
    bit     13   read-only   1 marks the register read-only
    bits 12:0    index       the register's index within the space

Four rules follow from the layout, each is directly testable, and the machine applies them in
the order below so that a program can predict which trap it gets.

1. A number whose privilege field is `%10` or `%11` raises the illegal-operand trap with
   subcode 7 at every privilege level, including supervisor, because a reserved privilege
   encoding names no level the machine can check an access against.
2. An access from a privilege level below the one bits 15 and 14 name raises the
   privileged-operation fault.
3. A `csr_write` to a number whose read-only bit is set raises the illegal-operand trap with
   subcode 4, and the register keeps its value.
4. A well-formed number that this machine does not implement raises the illegal-operand trap
   with subcode 3, on read and on write alike. Maize v1 read an undefined control-register index as zero and
   discarded a write to it; v2 traps instead, so a program built against a register the machine
   lacks stops at the access rather than running on with a plausible zero.

The order is observable. A number that is read-only and names supervisor, accessed at user
level, takes rule 2 and raises the privileged-operation fault, and it does not reach rule 3.

A read of an implemented register has no side effect on any other register. A write may have
the side effects the register's own entry states, and exactly one register in the base has
one: writing the paging root flushes cached translations.

### Index allocation

Index values `$0000` through `$0FFF` belong to the base, and this chapter assigns them. Index
values `$1000` through `$1FFF` belong to extensions, allocated in blocks of `$100` by the
extension registry, so an extension's state is reachable through the same three instructions and
is discoverable through the same feature bitmap. An unallocated extension index is a
well-formed unimplemented number and traps like any other.

### The base registers

The table below is the complete list of control and status registers in the base. Every
number not in it is unimplemented.

| Number | Name | Privilege | Access | Contents |
|:-------|:-----|:----------|:-------|:---------|
| `$0000` | `fcsr` | User | Read-write | The floating-point rounding mode and the sticky exception flags |
| `$2000` | `feature_bitmap` | User | Read-only | One bit per extension, set when this machine implements that extension |
| `$4000` | `status` | Supervisor | Read-write | The privilege level and the external-interrupt enable bit |
| `$4001` | `trap_stack` | Supervisor | Read-write | The full-descending stack pointer the machine pushes trap frames to |
| `$4002` | `trap_vector_base` | Supervisor | Read-write | The virtual address of the 256-entry vector table |
| `$4003` | `paging_root` | Supervisor | Read-write | The translation mode and the physical address of the root page table |
| `$4004` | `interrupt_enable0` | Supervisor | Read-write | Enable bits for causes 0 through 63. Bits 0 through 31 correspond to synchronous causes, which are never maskable, so those bits read as zero and a write that sets any of them raises the illegal-operand trap. |
| `$4005` | `interrupt_enable1` | Supervisor | Read-write | Enable bits for causes 64 through 127 |
| `$4006` | `interrupt_enable2` | Supervisor | Read-write | Enable bits for causes 128 through 191 |
| `$4007` | `interrupt_enable3` | Supervisor | Read-write | Enable bits for causes 192 through 255 |
| `$4008` | `syscall_provider` | Supervisor | Read-write | Bit 0 selects the syscall provider; every other bit is reserved and written as zero |
| `$4009` | `scratch` | Supervisor | Read-write | A scratch word the machine itself never reads or writes, held for the trap-entry register bootstrap |
| `$6000` | `interrupt_pending0` | Supervisor | Read-only | Pending bits for causes 0 through 63 |
| `$6001` | `interrupt_pending1` | Supervisor | Read-only | Pending bits for causes 64 through 127 |
| `$6002` | `interrupt_pending2` | Supervisor | Read-only | Pending bits for causes 128 through 191 |
| `$6003` | `interrupt_pending3` | Supervisor | Read-only | Pending bits for causes 192 through 255 |
| `$6004` | `halt_cause` | Supervisor | Read-only | Why the machine stopped, in the layout the trap-model chapter fixes |
| `$6005` | `boot_info` | Supervisor | Read-only | The physical address of the boot-information block |

Six of these registers carry rules that the table has no room for, and each rule is normative.
Wherever a rule in this chapter says that a write of a particular value raises the
illegal-operand trap, the trap carries subcode 6, the value the trap-model chapter assigns to
an invalid value written to a control and status register. A conformance test can therefore
derive the whole cause word for every one of these traps.

- `fcsr` holds the rounding mode and the five sticky exception flags in the layout the
  floating-point chapter fixes. It is user-accessible because user code performs
  floating-point arithmetic and has to be able to read its own flags.
- `feature_bitmap` reports extension presence for a fast check. The authoritative list, with
  each extension's version, lives in the boot-information block. Which bit belongs to which
  extension is fixed by the extension registry when the extension is ratified, alongside its
  escape page and its control-and-status-register block, so neither this chapter nor the
  extension-governance chapter carries a bit table. The registry is the single owner.
- `trap_stack` requires 16-byte alignment, and a write of a misaligned value raises the
  illegal-operand trap.
- `trap_vector_base` requires 2 KiB alignment, and a write of a value whose low 11 bits are
  not all zero raises the illegal-operand trap.
- `syscall_provider` replaces the two v1 opcodes that toggled the provider. Bit 0 selects
  between the providers the syscall surface defines, and a write that sets any other bit
  raises the illegal-operand trap.
- `boot_info` reports where the machine placed the block describing the memory size, the
  device inventory, and the extension list. Software reads that block rather than probing for
  memory, and the block's format belongs to the memory-model chapter.

At reset every writable register in the table holds zero except `status`, which holds `$1`,
the supervisor level with interrupts disabled. The read-only registers hold what the machine
has to report: `feature_bitmap` and `boot_info` are populated before the first instruction
executes, the pending registers are clear, and `halt_cause` is zero. The boot
chapter states the full reset contract, including the general registers and the program
counter.

Three kinds of state that other machines put in this space are deliberately absent from the
base. There is no faulting-address register and no fault-error register, because the trap
frame carries the address, the access kind, and the permission-versus-presence distinction.
There is no cycle counter and no retired-instruction counter, because performance state
belongs to the metering extension. There is no hart identifier, because the base machine is
single-hart.

## Address translation

The machine translates every virtual address a program uses, including instruction fetches,
through the mode the paging-root register selects. Translation is the only mapping between
virtual and physical addresses, and page-table reads themselves are physical accesses that are
never translated. A page-table read that names a physical address outside populated memory
therefore raises the physical-memory fault rather than a page fault, and so does an access
whose translation succeeded but whose resulting physical address is unpopulated.

### The paging-root register

The `paging_root` register carries the mode and the root table together.

    bits  3:0    mode      0 = bare, 1 = Sv48, 2 through 15 reserved
    bits 11:4    reserved, written as zero
    bits 63:12   root      bits 63 through 12 of the physical address of the root page table

A write whose mode field holds a reserved value, or whose reserved bits are not all zero,
raises the illegal-operand trap and changes nothing. Maize v1 forced the reserved bits to zero
and treated an unknown mode as bare; v2 rejects both, on the same mistake-proofing grounds
that make a reserved opcode trap.

In bare mode the physical address equals the virtual address, no page table is consulted, and
no access raises a page fault. Bare mode removes the page fault and nothing else. An access
whose physical address lies outside populated memory still raises the physical-memory fault,
cause 11, which the trap-model chapter defines and which every translation mode can deliver.
Bare mode is the reset state, so a machine with no kernel on it runs with translation off.

Every write to `paging_root` flushes every cached translation, whether or not the write
changes the value. A kernel that switches address spaces therefore needs no invalidation
instruction, and the flush is architectural rather than a property of any one implementation.

### Sv48 translation

In Sv48 mode the machine translates the low 48 bits of the virtual address through four levels
of page table. Bits 63 through 48 of the virtual address are ignored: they take no part in the
translation, and no canonical-form check rejects them.

The virtual address splits into five fields:

    bits 47:39   the level-3 index
    bits 38:30   the level-2 index
    bits 29:21   the level-1 index
    bits 20:12   the level-0 index
    bits 11:0    the offset within the page

A page table occupies one 4 KiB page and holds 512 entries of 8 bytes each, stored
little-endian. The walk begins at the physical address in the paging-root register's root
field, with the low 12 bits taken as zero, and reads the entry the level-3 index selects. At
each level the machine reads one entry and either descends to the table that entry names or
stops at a leaf.

### The page-table entry

A page-table entry is one word with this layout, unchanged from Maize v1 so that a kernel's
paging code ports across without a rewrite.

    bit     0    V   valid
    bit     1    R   readable
    bit     2    W   writable
    bit     3    X   executable
    bit     4    U   user-accessible
    bit     5    G   global
    bit     6    A   accessed
    bit     7    D   dirty
    bits 11:8    available to software
    bits 63:12   bits 63 through 12 of a physical address

The machine reads five bits and one field: V, R, W, X, U, and the physical address. It never
reads G, A, D, or the software-available bits, and it never writes any part of any entry. The
accessed and dirty bits are software-managed, which means a kernel that wants them maintains
them from its page-fault handler by mapping a page read-only or invalid until the first access
tells it what it wants to know.

An entry with V clear names no mapping. An entry with V set and at least one of R, W, and X
set is a leaf and names a page. An entry with V set and R, W, and X all clear is a non-leaf
and names the next table down, whose physical base is bits 63 through 12 of the entry with the
low 12 bits taken as zero.

A leaf above level 0 maps a superpage: 2 MiB at level 1, 1 GiB at level 2, and 512 GiB at
level 3. The physical address of the translated byte is the leaf's physical address field with
the virtual address's offset bits below that level's page boundary substituted in.

### What translation rejects

Translation fails in exactly six ways, and each one delivers a page fault. Three of them mean
no valid mapping exists in a structural sense, one means the walk ran out of levels, and two
mean a mapping exists but the access is not allowed.

- An entry with V clear names no mapping, at any level.
- A leaf with W set and R clear is a reserved encoding, and the machine rejects it as an
  invalid entry rather than honoring it as a write-only page.
- A leaf above level 0 whose physical address field has any nonzero bit below its level's page
  boundary is a misaligned superpage, and the machine rejects it as an invalid entry rather
  than aliasing it to an aligned address.
- A non-leaf entry at level 0 has nothing left to descend to, and the machine treats it as no
  valid mapping.
- A leaf that lacks the permission bit the access needs is a permission violation: a fetch
  needs X, a load needs R, and a store needs W.
- A leaf with U clear accessed from user level is a permission violation. Supervisor may reach
  a page whether U is set or clear, so a kernel can read and write a user page directly.

The first four conditions deliver the page fault with subcode 0, meaning no valid mapping was
found. The last two deliver it with subcode 1, meaning a mapping was found and the access
violates it. A handler that needs to know more inspects the page tables itself, which it can
do because it has the faulting address.

### Page-fault delivery

A page fault is delivered through the trap-model chapter's ordinary vectored path, with the
cause naming the access kind: cause 8 for an instruction fetch, cause 9 for a load, and cause
10 for a store. A block-memory instruction's reads deliver cause 9 and its writes deliver
cause 10.

Three properties of delivery are normative. The auxiliary word carries the faulting virtual
address exactly as the instruction computed it, including its low bits, so a handler learns
both the page and the offset. The captured program counter is the faulting instruction's own
address, because a page fault is fault-class and the instruction is meant to run again. The
faulting instruction has taken no architectural effect, or, for a block-memory instruction,
exactly the effect its restartability contract describes, so servicing the fault and returning
completes the access.

A page fault or a physical-memory fault raised while the machine is building a trap frame or
reading the vector table is a double fault, and the trap-model chapter says the machine halts
rather than recursing.

## The translation cache

The machine may cache translations it has performed, and the base assumes it does. The cache
is architecturally invisible except through one rule: a cached translation stays usable until
something invalidates it, and writing a page-table entry is not something that invalidates it.

Two consequences of that rule are worth stating plainly. Software that edits a live page table
and then invalidates gets the new translation on the next access. Software that edits a live
page table and does not invalidate may get the old translation, the new one, or the old one on
one access and the new one on the next, and all of those are conforming machine behavior. The
nondeterminism is bounded by the rule and by nothing else, so a program that depends on which
translation it gets is a broken program rather than a machine with a defect.

A cached translation carries the permission bits of the leaf it came from, and the machine
re-checks those bits against the access kind and the current privilege level on every use. A
change of privilege level therefore requires no invalidation, and a cached user page does not
become reachable from user mode because the kernel touched it.

Three events invalidate. A write to the paging-root register discards every cached
translation, as the register's entry above states. The two maintenance instructions discard
translations explicitly:

- `tlb_invalidate_all` discards every cached translation.
- `tlb_invalidate_address rs` discards any cached translation for the page containing the
  virtual address in rs.

Both instructions are privileged, and executing either at user level raises the
privileged-operation fault. Neither raises a fault for an address that has no cached
translation, for an address that is not mapped at all, or while the machine is in bare mode.
A machine that caches nothing satisfies both instructions by doing nothing at all, and it is
fully conforming.

A kernel that changes a single page-table entry follows the change with
`tlb_invalidate_address` naming any address in the affected page. A kernel that rewrites many
entries follows them with one `tlb_invalidate_all`. A kernel that switches address spaces
writes the paging-root register and needs neither.

## Conformance notes

Every property below is directly testable by a binary, and a conforming machine exhibits all
of them.

- Each of the seven privileged instructions raises cause 4 at user level and succeeds at
  supervisor level.
- A `csr_read` of `$4000` raises cause 4 at user level and returns the status register at
  supervisor level, and a `csr_read` of `$0000` succeeds at both levels.
- A `csr_write` at supervisor level to any number whose bit 13 is set raises cause 1, and the
  register's value is unchanged afterwards. The same write at user level to a number naming
  supervisor raises cause 4 instead, since rule 2 is applied before rule 3.
- A `csr_read` or `csr_write` of a well-formed unimplemented number raises cause 1 at
  supervisor level rather than returning zero.
- A `csr_write` of a value with a reserved mode field, or with a nonzero bit in bits 11
  through 4, to `paging_root` raises cause 1 with subcode 6 and leaves translation as it was.
- In bare mode every address translates to itself and no access raises a page fault, including
  an access to an address for which no page table exists, while an access outside populated
  physical memory still raises cause 11.
- Under Sv48 a fetch from a page whose leaf lacks X raises cause 8 with subcode 1, a load from
  a page whose entry has V clear raises cause 9 with subcode 0, and a store to a page whose
  leaf lacks W raises cause 10 with subcode 1, each with the exact faulting virtual address in
  the auxiliary word.
- A leaf with W set and R clear, and a level-1 leaf whose physical address field has a nonzero
  bit below bit 21, each raise a page fault with subcode 0.
- A user-mode access to a leaf with U clear faults, and a supervisor-mode access to the same
  leaf succeeds.
- A 2 MiB superpage leaf at level 1 translates every address in its range, and the physical
  address is the leaf's address field with the low 21 bits of the virtual address substituted.
- After a page-table entry is changed and `tlb_invalidate_address` names an address in that
  page, the next access uses the new entry.
- After the paging-root register is written with its own current value, an access whose entry
  was changed since the last invalidation uses the new entry.
