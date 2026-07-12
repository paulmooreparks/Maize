# Chapter 4: Memory Model

This chapter is normative. It fixes the address space, byte order, the sparse no-fault
memory semantics, alignment behavior, and the process-start block.

## 4.1 Flat 64-bit address space

Maize has a single linear address space of **2^64 bytes**, addressed `$0` through
`$FFFF_FFFF_FFFF_FFFF`. There is no segmentation in v1.0: pointers, PC, and SP are all full
64-bit addresses. The machine is **unbounded**: there is no fixed RAM ceiling, so the top
of the stack is a chosen top-of-space constant (section 4.5), not a derived RAM limit.

Devices are **not** in this space. There is no memory-mapped I/O: no device register is
reachable through LD / ST / CP, and a port access never touches memory. The device surface
is a disjoint 16-bit port space (Chapter 11).

## 4.2 Byte order

Memory is **little-endian**. A multi-byte value is stored with its least-significant byte
at the lowest address. Immediate operands in the instruction stream are likewise
little-endian (Chapter 6), and the object/executable file formats match. Reading an 8-byte
slot back with `LD @addr Rn` reconstructs the value that a `ST Rn @addr` wrote, and the
process-start block slots (section 4.5) are read back this way.

## 4.3 Sparse, no-fault memory (allocate-on-write, read-zero)

Memory is **sparse** and **lazily zero-filled**:

- A read of never-written memory returns **0**. There is no fault, no EFAULT, no page
  fault in the v1.0 flat model.
- A write to never-written memory **allocates** backing storage for that region on first
  touch, zero-filled, and stores the value.

This is a defined, non-trapping outcome (Chapter 10), not a gap in the trap taxonomy. The
absence of a memory-access fault here is deliberate. Segment / bounds enforcement (trap
cause 5) is a separate, reserved future path (Chapter 12); in the v1.0 flat model no access
is ever out of bounds.

*Implementation note (informative).* The reference VM backs memory with 256-byte
allocation blocks in a map keyed by block address, allocating a block on first write and
returning zero for any block never allocated. The 256-byte block is an implementation
detail, not an architectural boundary; software must not rely on the block size, only on
the read-zero / allocate-on-write semantics above.

## 4.4 Alignment: defined-allow

Multi-byte accesses have **no alignment requirement**. A load or store of any width may sit
at any address; it is stitched byte-wise across allocation blocks with no trap and no
performance-visible architectural penalty in the behavioral model. Misalignment is a
defined-allow outcome, not undefined behavior and not a trap. No trap vector is spent on
alignment.

A load reads exactly as many bytes as its destination subregister holds and no more, so it
never over-reads past its source address (Chapter 7 section 7.1).

## 4.5 Process start and the process-start block

At a fresh VM invocation Maize builds a System V-style process-start block at the top of
the address space and points RS (SP) at its base. There is no ELF auxiliary vector. This
makes a C `main(int argc, char **argv, char **envp)` callable from the first instruction.

The block occupies the top of the address space, ending at `$FFFF_FFFF_FFFF_FFF8`; the
guest image loads at address 0, so the two never overlap. From RS upward, every slot is 8
bytes, little-endian (read back through `LD @`):

    RS + 0                  argc
    RS + 8                  argv[0]     -> address of argv[0]'s string
    ...                     argv[argc-1]
    RS + 8 + argc*8         0           (argv NULL terminator)
    next 8                  envp[0]     -> address of envp[0]'s string
    ...                     envp[envc-1]
    ...                     0           (envp NULL terminator)
    [higher addresses]      the NUL-terminated argument and environment strings, packed
                            in order and ending at the top of the address space

Each `argv[i]` and `envp[j]` holds the absolute address of its NUL-terminated string.
`envp` is always present and NULL-terminated even with zero environment entries, so
`main(argc, argv, envp)` is always well-formed; a `main(void)` simply ignores it. The
initial register contract that accompanies this block (RS, RP, RB, the general registers,
and RF) is in Chapter 2 section 2.6 and Chapter 9.

The stack is **full-descending**: PUSH and CALL pre-decrement RS before writing, so at
process start RS points one slot past the top usable stack slot (it points at argc), and
the first guest push moves RS down into the free region just below the block. No
stack-pointer wraparound is relied upon.

## Sourcing

- Flat-64 and no-MMIO: README "Special-purpose Registers" flat-64 note and "Devices and
  port I/O"; Chapter 11 (`docs/spec/device-surface.md`).
- Sparse allocate-on-touch, read-zero, 256-byte block: `src/maize_cpu.h` `memory_module`
  (block_size `$100`, the `memory_map`, lines ~373-408) and `src/cpu.cpp` memory read/write
  paths; `docs/spec/trap-model.md` "Defined non-trapping behavior".
- Little-endian and misaligned defined-allow: README "Instruction Description" and the
  byte-wise stitching in `memory_module`; `docs/spec/trap-model.md`.
- Process-start block and the reset register contract: README "Process start" and
  "Process-start block"; Chapter 9.
