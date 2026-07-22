# The mzvm / quesOS boundary

**Status: ratified policy, 2026-07-23.** This is a decision of record, not a proposal. It governs what goes into the virtual machine (mzvm, the sources under `src/`) versus what goes into the operating system (quesOS, guest code under `os/quesos/`). Companion to the North Star and to `quesos-enlightened-layer.md`.

## The principle

mzvm implements the machine. quesOS is software that runs on the machine. The dividing line sits exactly where the hardware/software line falls on a real computer. The VM is a CPU, an MMU mechanism, a set of devices, and a boot path. The OS is processes, scheduling, memory policy, the filesystem, syscall semantics, and IPC.

## The governing test

Every candidate for the VM faces one question. Would an independent second VM implementer have to reimplement this to run quesOS? If yes, it is machine, it goes in mzvm, and it becomes part of the frozen spec contract that every conforming VM must reproduce. If it is OS policy, it belongs in the guest as bytecode, and that second implementer gets it for free by running the same `.mzx`.

Grow quesOS freely, because it is portable, snapshot-able, borrowable, polyglot guest code. Grow mzvm reluctantly, because every addition is frozen contract, host state that does not travel with a snapshot, and machine that a second implementer must clone.

## Why this project holds the line harder than most

Three reasons, and the first two follow from decisions already ratified.

The spec is the product. Anything in mzvm is part of the machine contract the conformance suite pins and every third-party VM must reimplement, so welding OS policy into the VM defeats the "implement your own Maize VM in a weekend" story.

The enlightened-layer vision requires it. "The machine as a value" depends on a snapshot of guest RAM, registers, and device state being the whole running system. If quesOS state lives in the host heap inside the VM, a snapshot does not capture the OS cleanly and migration across hosts breaks. Fat-guest-OS is a precondition for snapshot, fork, and migration, not a preference.

The two-ABI plan is an OS construct. Both the Linux-compatible ABI and the native quesOS ABI are things the OS decides. The VM offers one small, stable trap primitive and lets the OS assign meaning to syscall numbers. Baking one ABI into the machine forecloses the other.

## Policy: what belongs in mzvm

The machine surface, and nothing beyond it:

- Instruction execution, the frozen v1.0 ISA.
- The MMU mechanism: the page-table walk, the software TLB, and fault delivery. The VM knows how to translate. It does not decide who gets which pages.
- Trap and interrupt delivery, including the cause-7 SYS trap.
- Devices: console, framebuffer, keyboard, timer, block, the coming NIC, and an entropy source. These are the machine's I/O and the host bridge, and they are where real host interaction lives, because the guest cannot open a host file, drive a real terminal, touch the network, read the clock, or draw entropy on its own.
- A minimal boot path that loads the OS artifact.

The nondeterministic edges all live here, as devices. Real files, the real terminal, the real network, the real clock, and real entropy are host interactions the VM services, and they are the recorded inputs the determinism story is built around. Native implementation at the device boundary is a necessity, not a shortcut.

## Policy: what belongs in quesOS

Everything that is how this particular OS behaves, as compiled guest code:

- Processes and exec, and the loader policy above the minimal boot path.
- Scheduling.
- Memory policy: page allocation, per-process address spaces, and fork semantics on top of the VM's translation mechanism.
- The fd table, the VFS, path resolution, and permissions.
- Signals and IPC.
- Syscall semantics, and both ABIs.
- The native concurrency model (the Erlang-blueprint actor layer), which is OS policy rather than machine mechanism.

## The syscall split

A syscall is not one thing. It is three layers, and they divide across the boundary. Take `write(fd, buf, n)`. Trap delivery is machine, so mzvm owns it. Policy, meaning the fd table, permissions, and blocking behavior, is OS, so quesOS owns it. The actual byte movement to a console or a socket is a device operation, so mzvm owns that too.

So the primitive the VM exposes is a device operation, not a syscall. quesOS does the policy and then calls a device operation to perform real I/O. The `SETSYSG`/`CLRSYSG` dual path is the mechanism that carries this. The native provider in `src/sys.cpp` is a bootstrap and compatibility shim plus the device layer. The guest cause-7 handler is the real OS. The migration direction is that policy moves from native into the guest over time, while device access stays native because it must touch the host. The end state is that native code implements devices, not syscalls.

## The one allowed exception, and its caveat

A semantically transparent performance escape hatch that the interpreter cannot afford is allowed in the VM. The `$F4`/`$F5` bulk memcpy and memset syscalls are the standing example, because doing them one instruction at a time in the interpreter is about a thousand times too slow, and they compute exactly what the guest could compute itself.

The caveat is a DIRT one. Baking a performance shortcut into a syscall makes it a permanent ABI commitment, because the guest calls it by number and removing it later is a breaking change. A performance shortcut baked into a JIT optimization stays internal and replaceable. Prefer the JIT path for speed, reserve native acceleration for operations the guest genuinely cannot express itself, and where a fast operation is truly a machine primitive, consider giving it an instruction rather than a syscall so its home is the ISA rather than the OS ABI. With the JIT now in progress, do not add new native performance syscalls until its handling of memory operations is known.

## Language per layer

Match the language to the layer.

- The boot stub and the trap trampoline stay in MAZM, kept tiny, because the register save and restore prologue and the cause-7 entry and exit need exact control and are small.
- The kernel proper, in the Linux-compatible era, stays in C. cproc is available now, C is borrowable so Unix code can be stolen, and the existing `quesos.c` works. Rewriting what runs is not on the table.
- The native enlightened core, which is greenfield and arrives after the LLVM backend, is the right place to introduce a safer language, written fresh against the native ABI rather than as a rewrite. Zig and Rust are both credible, and the choice opens when maize-108 lands rather than now.

## The rule, in one line

If a second implementer would have to rebuild it to run quesOS, it is the machine and it belongs in mzvm. Otherwise it is the OS and it belongs in the guest.
