# quesOS: The Enlightened Layer

> Maize can be the first computer where the whole running system is a deterministic, forkable, meterable, serializable value, and quesOS the first OS built to exploit that instead of fighting hardware that refuses to provide it.

**Status: vision, not committed scope.** This document records a direction ratified by the operator on 2026-07-23, and it sits deliberately after 1.0 and behind the JIT in sequence. Nothing here changes the 1.0 plan, which still ships quesOS as the Unix-compatible OS described in the North Star. This is what quesOS grows into once the machine runs borrowed software at near-native speed, and it is now the reason quesOS exists rather than a nice-to-have on the side. quesOS is the chosen operating system for Maize, and the ideas below are what it is chosen to enable.

Read this as a companion to the North Star document, not a replacement. The North Star says borrow the world and reach Linux. This says: having borrowed the world, build something on top of it that no borrowed OS could be.

## The thesis: the OS graveyard is full of designs that would work here

Almost every idea in this document has been tried before in operating-system research, and most of the attempts died. Single-level stores, capability kernels, deterministic operating systems, orthogonally persistent systems, tagged and capability-checked memory. The ideas were good. The implementations are mostly gone.

They died for two reasons, and essentially only two. They fought the hardware, which refused to make their primitives cheap or correct, and they fought compatibility, because nobody would surrender the Unix software world to get them.

Maize removes both obstacles at once, which is the entire opportunity. We define the hardware, so the OS-friendly primitives are cheap and correct by construction rather than emulated on top of silicon that was built for something else. And the North Star already chose a layered answer to compatibility, where the Linux ABI borrows the world and a native layer offers what Linux never will. The value is not any single feature below. It is that the graveyard of good OS ideas is full of designs that would simply run on this machine, and we are in a position to build the machine to fit them.

## The architectural key: two ABIs, one machine

Every feature in this document pulls away from Linux compatibility, and the North Star's whole strategy depends on keeping it. That tension has a clean resolution, and it is the load-bearing decision of the whole design.

quesOS presents two ABIs on one machine. The Linux-shaped ABI carries the borrowed world unchanged, so oksh and sbase and eventually a whole distro's userland run without knowing anything unusual is underneath them. A second, native quesOS ABI exposes the machine's real capabilities to programs that opt into them. A binary is either an ordinary Linux program or a Maize-native one, in the same way a paravirtualized guest carries "enlightenments" it uses only when it knows it is virtualized.

This is the DIRT resolution the operator already ratified, applied at the scale of an operating system. Borrow to bootstrap, build the unique layer on top, and let the native ABI be the contract where the real value lives. Because the native features live in a separate ABI, none of them block 1.0 and none of them break the borrowed world. quesOS ships Unix-compatible first, and the enlightened layer grows on the same kernel afterward without a rewrite.

## The five pillars

### 1. The machine as a value

This is the crown jewel, and it falls directly out of deterministic execution over small, fully specified state. When the whole machine is a deterministic function of its inputs, the entire running system becomes a value you can copy, diff, fork, move, and replay. Real hardware cannot offer this at any price, because caches, speculation, asynchronous interrupts, and wall-clock time make a modern CPU irreproducible from moment to moment.

As first-class kernel primitives rather than bolted-on tooling, that gives us:

- Checkpoint and restore of any process, or the whole OS, that is correct by construction instead of the heroic edge-case engineering that CRIU is on Linux.
- System-wide time-travel debugging with no instrumentation overhead, because we replay a deterministic machine from a snapshot plus an input log rather than reconstructing history. `rr` gives one process this on Linux at real cost; here the kernel gives it to everything essentially for free.
- Transactional syscalls, where a process forks the world, attempts something risky, and discards the branch on fault or regret, which makes "undo" a native verb of the operating system.
- Live migration across radically different hosts, so a process starts on a server, moves to a phone mid-execution, and finishes in a browser tab, because the ABI is the only contract and the snapshot is architecture-neutral bytecode plus state.
- Replication by construction, which is the wild one. Feeding N copies of the machine the same input log produces bit-identical state on every copy. The hardest problem in distributed systems, agreeing on state across replicas, becomes a property of the VM, so Byzantine-fault-tolerant and audit-grade replicated services can be ordinary quesOS programs rather than feats of consensus engineering.

The JIT is what turns this from a curiosity into a platform. Snapshotting a toy is a demo. Snapshotting a machine that runs real software at near-native speed is infrastructure.

### 2. Cycle-metering as the OS currency

The cost model, once it lands, measures work in ISA-defined cycles rather than wall-clock time, and that gives the operating system a unit of account that is exact, reproducible, and independent of how fast the host happens to be.

Scheduling can be genuinely fair, because fairness is denominated in a currency the machine defines rather than in wall-clock slices that drift with cache behavior. Accounting becomes exact and auditable, so a process's cost is a reproducible number, which matters for any metered-compute or billing story. A process can be handed a hard cycle budget and stopped precisely when it is spent, turning the security world's resource-exhaustion defense into a first-class scheduling primitive. And the machine gains an unusual freedom over time, where a process can run detached from the wall clock, faster or slower or paused, deterministically, so the question of what a program does over a billion cycles is answered without waiting for a billion cycles to pass.

### 3. Memory safety in the ISA

We own the instruction set, and it is frozen but extensible through the reserved space. That means memory safety can live in the architecture itself, the way CHERI and ARM's Morello are only now trying to retrofit into real silicon. Capability pointers, bounds carried in the pointer, and tags the machine refuses to forge are all things a defined machine can simply have.

The consequence is that safe C stops being a contradiction. Every language that compiles to the ISA inherits memory safety from the machine rather than from its own runtime, so a C library, a Rust program, and a Lua interpreter are all protected by one enforcement mechanism checked in one place. The JIT is what keeps this affordable, because it can prove many checks redundant and elide them, doing in software what CHERI needs new hardware to do.

### 4. The polyglot world with no seams

One ISA and one calling convention means the language boundary, which is a tax everywhere else, mostly disappears. A Rust process calling a Lua library calling a C routine is just calls at the ABI level, with no FFI shims and no marshalling, because there is a single ABI underneath all of them.

The effects reach past convenience. OS services can be written in whichever language suits each one and still interoperate directly. The debugger, the profiler, and the introspection tools all work at the ISA level, so there is one debugger for every language instead of one per runtime. Packages ship as architecture-neutral bytecode that runs anywhere a Maize VM runs, which retires "works on my machine" as a category of problem.

### 5. A verifiable, capability-secure trust base

Small plus fully specified equals tractable to reason about. A capability-secure kernel in the seL4 or Fuchsia lineage, where the syscall surface is deny-by-default capabilities with no ambient authority, is a realistic target here rather than a decade-long research program, because the machine underneath it is specified and comprehensible. Formal verification of the kernel against the spec is on the table for the same reason. Nested isolation is nearly free, since every process already runs inside a VM and forking the machine is cheap, which makes containers a triviality with perfect boundaries rather than the leaky abstraction they are on Linux.

## Erlang as a validating case

Erlang and its BEAM runtime deserve a section of their own, because Erlang independently invented in software several of the things this document proposes to provide in hardware, which makes it both a validation of the direction and a natural forcing function. BEAM meters work in "reductions" to drive preemptive scheduling, which is a software cost model that maps onto Maize's cycle model. Its share-nothing processes with per-process heaps map onto capability isolation. Its distribution model is location-transparent message passing, which is the same shape as replication by construction. Its hot code loading and supervision trees pair naturally with snapshot, rollback, and transactional recovery.

There are two paths, and they compose rather than compete. One borrows BEAM through the C door and gets Elixir, Gleam, and the rest of the family with it. The other lifts BEAM's ideas into the native quesOS ABI, so cheap share-nothing actors, kernel-level message passing, and supervision become an OS-level concurrency model any language reaches through the ABI.

Operator decision, 2026-07-23: the blueprint comes first. Erlang's process model shapes the design of the native quesOS concurrency layer in the near term, ahead of any BEAM port, because getting that model right is the foundation and a clean native actor ABI is exactly what a later BEAM port would want to sit on. Porting BEAM itself becomes an optional follow-on rather than the entry point, taken up only if hosting the BEAM language family earns its keep once the native layer exists. Erlang is treated here as a design source for quesOS, not first as a language to host, which is the borrow-then-build doctrine run in the build-informed-by-the-borrow direction.

## Tensions and honest caveats

This direction is not free, and the caveats belong in the same document as the dream.

The borrow-versus-build tension is real and permanent, and the two-ABI split is the answer, but it means carrying two contracts and resisting the temptation to let the enlightened features leak into the Linux ABI where they would break compatibility. Determinism trades against SMP throughput, since a bit-reproducible machine runs its schedulers in a defined order rather than racing across cores, so the deterministic guarantees are a mode with a throughput cost rather than a free lunch, and that mode has to be opt-in. The whole vision leans on the JIT, and without near-native speed most of these features are interesting rather than usable, which is exactly why the sequence puts them after it. And every capability offered through the native ABI is new surface to secure, so the small-trust-base pillar is a discipline to maintain rather than a property we get for nothing.

## Where this sits in the plan

Nothing here is scoped as work yet, and that is deliberate. The design comes first, this document is the first artifact of it, and concrete cards follow once the shape is settled and the JIT is close enough to make the features real. The near-term board is unchanged.

What this direction is not is unmoored from the work already underway. It is the convergence point of threads that already exist on the board. The Agent Control Plane is snapshot, fork, and introspection. The Security workstream is the capability trust base. The Compilers and Language Ports work is the polyglot world. The still-open cost model is the metering currency. The dream is what those become when they stop being treated as separate features and are recognized as facets of one machine.
