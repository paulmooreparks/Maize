# The Calling Convention

This chapter is normative for software that wants to interoperate. It fixes the register
roles, the stack-frame shape, the argument and return-value rules, the variadic-argument
mechanics, and the syscall register contract that every Maize v2 object file, library, and
kernel entry agrees on.

Nothing in this chapter is enforced by the machine. The architecture names a stack pointer
in exactly one place, the trap-stack control and status register of the trap model, and it
names a link register in exactly one place, the `call` and `return` pair of the
instruction-inventory chapter. Everything else here is agreement between translation units,
which is why a violation shows up as a corrupted program rather than as a trap. The
conformance section at the end says what a test can and cannot pin as a result.

## Register roles

The ABI assigns every one of the 32 registers a role and a saver. The canonical names `r0`
through `r31` are always valid; the ABI names in the table are assembler aliases for the same
registers, and the disassembler emits the ABI names.

| Number | ABI name | Role | Saver |
|:------:|:---------|:-----|:------|
| r0 | `zero` | Reads as zero, discards writes. | Architectural, never written |
| r1 | `tp` | Thread pointer, owned by the runtime. | Invariant across calls |
| r2 | `a0` | First argument; first result word; hidden result pointer. | Caller |
| r3 | `a1` | Second argument; second result word. | Caller |
| r4 | `a2` | Third argument. | Caller |
| r5 | `a3` | Fourth argument. | Caller |
| r6 | `a4` | Fifth argument. | Caller |
| r7 | `a5` | Sixth argument. | Caller |
| r8 | `a6` | Seventh argument. | Caller |
| r9 | `a7` | Eighth argument. | Caller |
| r10 | `t0` | Temporary; dynamic syscall number. | Caller |
| r11 | `t1` | Temporary. | Caller |
| r12 | `t2` | Temporary. | Caller |
| r13 | `t3` | Temporary. | Caller |
| r14 | `t4` | Temporary. | Caller |
| r15 | `t5` | Temporary. | Caller |
| r16 | `t6` | Temporary. | Caller |
| r17 | `t7` | Temporary. | Caller |
| r18 | `t8` | Temporary. | Caller |
| r19 | `t9` | Temporary. | Caller |
| r20 | `s0` | Saved register. | Callee |
| r21 | `s1` | Saved register. | Callee |
| r22 | `s2` | Saved register. | Callee |
| r23 | `s3` | Saved register. | Callee |
| r24 | `s4` | Saved register. | Callee |
| r25 | `s5` | Saved register. | Callee |
| r26 | `s6` | Saved register. | Callee |
| r27 | `s7` | Saved register. | Callee |
| r28 | `s8` | Saved register. | Callee |
| r29 | `fp` | Frame pointer when one is used, otherwise a ninth saved register. | Callee |
| r30 | `sp` | Stack pointer. | Callee |
| r31 | `ra` | Link register, written by `call`. | Caller |

The partition is by decade so that a programmer can recover it from memory: r2 through r9
carry arguments, r10 through r19 are temporaries a call destroys, r20 through r29 survive a
call, and r30 and r31 are the two pointers control flow depends on. Ten registers survive a
call as general-purpose values (r20 through r29), and two more are invariant for a different
reason (`tp` belongs to the runtime, and `sp` is restored by the epilogue rather than saved
by the prologue).

Three of the roles need a sentence each. The thread pointer `tp` is never an argument and is
never allocated by a compiler; the thread library establishes it and every function leaves it
alone, so a callee that clobbers it breaks the whole thread rather than one call. The frame
pointer `fp` is optional, and a function that does not need one uses r29 as an ordinary saved
register, with no marker distinguishing the two uses. The link register `ra` is caller-saved
because `call` overwrites it unconditionally, so any function that itself calls must place
`ra` in its own frame before the first call and reload it before `return`.

## The stack

The stack grows downward, from higher addresses toward lower ones. Allocating n bytes of
frame subtracts n from `sp`, and releasing them adds n back. No instruction pushes or pops,
so every stack adjustment is an explicit `add` or `subtract` on `sp`, and every stack access
is an ordinary `load` or `store` through `sp` or `fp`.

The value in `sp` is a multiple of 16 at every `call` instruction, at every function entry,
and at every `return`. Because a frame adjustment happens in one instruction and the required
alignment holds on both sides of it, every frame is a multiple of 16 bytes, with the
prologue inserting up to 8 bytes of padding when the contents need less. Software keeps `sp`
aligned and valid at all times rather than only at call boundaries, because a trap, a
debugger, or a profiler can inspect the stack at any instruction boundary, and none of them
can be told to wait for the prologue to finish.

There is no red zone. The bytes below `sp` belong to nobody, and a trap handler, a signal
delivery, or a debugger agent may write them at any instruction boundary. A leaf function
that needs scratch memory allocates a frame for it like any other function.

The 16-byte alignment is wider than any base data type needs, and it is deliberate headroom:
a future vector extension can define a 16-byte-aligned type without a flag day, because every
existing frame already satisfies the alignment.

### Frame layout

A frame occupies the addresses below the value `sp` held when the function was entered. That
entry value is the frame's canonical address, and it is what `fp` holds in a function that
uses a frame pointer. Within a frame the regions appear in this order, from the highest
address to the lowest.

- Incoming stack arguments, at the frame address and upward, allocated and written by the
  caller.
- The saved link register, then the saved frame pointer, in the two words immediately below
  the frame address, present only in a function that saves them.
- The other saved registers, in any order the function chooses.
- Local variables, spill slots, and compiler temporaries.
- The outgoing stack argument area, at `sp` and upward, sized to the largest call the
  function makes.

Two consequences of that order are worth stating outright. The outgoing argument area sits at
the bottom of the frame so that at the moment of a `call` it begins exactly at `sp`, which is
the frame address the callee will see, and so a function allocates it once in its prologue
rather than adjusting `sp` around each call. The saved link register sits directly below the
frame address so that an unwinder that knows the frame address knows where the return address
is without reading any other metadata.

### The frame pointer and unwinding

A function that uses a frame pointer stores the caller's `fp` and its own return address at
fixed offsets, and sets `fp` to the frame address, which is the value `sp` had at entry. The
saved link register lives at `@fp-$8` and the caller's saved frame pointer at `@fp-$10`, so a
backtrace walks the chain by reading those two words and repeating. A function that omits the
frame pointer is unwound from the metadata its object file carries, and the shape of that
metadata is fixed by the Maize v2 object format and linking specification, at
`docs/spec-v2-toolchain/object-format.md`, which stands outside this specification and carries a
version line of its own. That document places the metadata in an `.eh_frame` section and fixes
what DWARF call-frame information leaves to a processor supplement, including the DWARF register
number of every register and the return-address column. Nothing here is left undefined by that
division, because the metadata is a note about tooling and every rule this chapter states holds
without it.

The prologue and epilogue of a frame-pointer function are fixed sequences. A 32-byte frame
looks like this:

    subtract sp $20 sp        ; allocate 32 bytes, alignment preserved
    store ra @sp+$18          ; saved link register at frame address minus 8
    store fp @sp+$10          ; saved frame pointer at frame address minus 16
    add sp $20 fp             ; fp becomes the frame address
    ; ... body ...
    load @sp+$10 fp
    load @sp+$18 ra
    add sp $20 sp
    return

The frame adjustment is a single instruction for any frame up to two gigabytes, because an
ALU immediate is a 32-bit value sign-extended to the operation width. A function needing more
than that materializes the size with an immediate move and subtracts a register.

## The C type mapping

Argument classification is stated in terms of object sizes, so the sizes have to be fixed
somewhere, and this is that place. Maize v2 is an LP64 target: `char` is one byte and is
signed, `short` is a quarter-word, `int` is a half-word, `long`, `long long`, and every
pointer are a full word, `float` is binary32, `double` and `long double` are both binary64,
and `_Bool` is one byte holding 0 or 1. Every scalar's alignment equals its size. A structure
or union is aligned to the strictest alignment among its members, and its size is a multiple
of that alignment.

Maize has no wider floating-point format, so `long double` is binary64 rather than an
extended type. Software that needs more precision uses a software format, which the ABI
treats as an ordinary aggregate.

### Narrow values in registers

A value narrower than a word occupies the low bits of its register, and every remaining bit
of that register is zero. This holds for signed and unsigned types alike, so a signed `int`
argument of -1 arrives as `$00000000FFFFFFFF` rather than as `$FFFFFFFFFFFFFFFF`.

Zero-extension is chosen because the machine already produces it. Every `.h` arithmetic
instruction writes its half-word result zero-extended into the full destination, so a value
computed by half-word arithmetic is in ABI form with no fixup, and a `load.zh` or `load.zb`
delivers an argument in ABI form directly. The one cost this imposes is visible and small:
the compare and branch families have no half-word forms, so a signed comparison of two
half-word values first widens each with `extract.sh rN.h0 rM`, one instruction per operand.
Unsigned comparison of narrow values needs no widening at all, and the `.h` arithmetic,
shift, and divide instructions read their sources' low halves and ignore the upper bits, so
they consume ABI-form values unchanged.

## Argument passing

The caller classifies arguments in a single left-to-right walk over the argument list, with
no reordering and no second pass. Eight argument registers, r2 through r9, are available at
the start of the walk, and each argument consumes zero, one, or two of them.

1. An argument of an empty aggregate type consumes no register and no stack space.
2. An integer, a pointer, or a `_Bool` consumes one register, holding the value in ABI form.
3. A binary64 value consumes one register, holding the IEEE 754 bit pattern in the full word.
   A binary32 value consumes one register, holding the bit pattern in the low half-word with
   the upper half zero. There is no separate floating-point argument class, because the
   machine has no separate floating-point register file.
4. An aggregate of 8 bytes or fewer consumes one register, holding the aggregate's memory
   image in the low bytes with the remaining bytes zero.
5. An aggregate larger than 8 bytes and no larger than 16 bytes consumes two registers,
   holding the first 8 bytes of the memory image in the first register and the rest in the
   second, with any unused high bytes of the second register zero.
6. An aggregate larger than 16 bytes is passed by reference. The caller places a copy of the
   object in its own frame, aligned as the type requires, and passes the copy's address as an
   ordinary pointer argument consuming one register. The callee may modify the copy freely,
   and the copy's lifetime ends when the call returns.

Two rules govern the transition to the stack, and together they make classification a
function of the argument list alone. An argument that needs more registers than remain is
passed entirely on the stack rather than split between registers and memory, and once any
argument has been assigned to the stack, every later argument is assigned to the stack as
well, even one that would still fit in a register. No argument is ever back-filled into a
register that a larger argument skipped over.

The little-endian memory-image rule in items 4 and 5 means the first declared member of a
structure occupies the low bytes of the first register, which is exactly the layout that
storing the register to memory produces. A callee that finds it easier to work in memory
therefore stores the argument registers into a frame slot and reads fields from there, with
no shuffling.

### Arguments on the stack

The stack argument area begins at the frame address, which is the value of `sp` at the
`call`. Each stack argument is placed at an offset that is a multiple of 8 bytes, or a
multiple of 16 when the argument's type requires 16-byte alignment, with padding bytes
inserted before it to reach that offset. An argument occupies the number of whole 8-byte
slots its size requires, rounded up, and the padding in the final slot of an odd-sized
argument is zero.

A narrow scalar on the stack follows the register rule: the caller writes the whole 8-byte
slot, with the value zero-extended into it. Writing the whole slot rather than only the
significant bytes is what lets a callee load any argument with a plain word `load` when it
does not care about the declared type, and it is what makes a variadic slot walk uniform.

An aggregate passed by reference occupies one slot holding the pointer, exactly as it would
occupy one register.

## Return values

The result of a function travels back in the same registers the first two arguments arrived
in, classified by the same rules.

- A result of 8 bytes or fewer, of any scalar or aggregate type, returns in `a0`, in ABI
  form.
- A result larger than 8 bytes and no larger than 16 bytes returns in `a0` and `a1`, the
  first 8 bytes of the memory image in `a0` and the rest in `a1`, with unused high bytes of
  `a1` zero.
- A result larger than 16 bytes returns through memory the caller provides. The caller
  allocates an object of the result type, aligned as the type requires, and passes its
  address as a hidden first argument in `a0`, shifting every declared argument one register
  to the right. The callee writes the result through that pointer and returns the same
  pointer in `a0`.
- A function returning `void` leaves `a0` and `a1` holding values this ABI does not define,
  and its caller reads neither.

Returning the hidden pointer in `a0` costs the callee one register copy at most and it saves
the caller from keeping the address alive across the call, which matters most at the call
sites a compiler generates for chained expressions. The hidden argument occupies a real
argument register, so a function with eight declared arguments and a large result passes its
last declared argument on the stack.

## Variadic functions

A variadic call passes every argument of the variadic tail on the stack, no matter how many
argument registers are still unused. The fixed arguments, meaning those the prototype
declares before the ellipsis, are classified by the ordinary rules and normally land in
registers; classification of the tail starts on the stack at the frame address and continues
upward by the stack rules above.

Putting the whole tail in memory buys two things worth more than the register traffic it
costs. A `va_list` becomes a single pointer with no register save area behind it, so
`va_start` is one instruction and no variadic prologue spills eight registers that a typical
call never uses. It also removes the classification asymmetry that makes register-based
variadic ABIs subtle, because the callee never has to know how many arguments arrived in
registers.

A call to a variadic function therefore requires the prototype to be in scope, which C
already requires of any variadic call.

### Default argument promotions

Arguments in the variadic tail are promoted before they are placed. An integer type narrower
than `int` is promoted to `int` and occupies one slot, zero-extended into the whole slot as
the narrow-value rule requires. A `float` is promoted to `double` and occupies one slot. Every
other type is placed unpromoted, and an aggregate follows the same size rules it would follow
as a fixed argument, so an aggregate larger than 16 bytes travels by reference and occupies
one pointer-sized slot.

### The va_list type and its operations

A `va_list` is one word holding the address of the next unread variadic slot. It is an
ordinary value that can live in a register, and it is copied by copying the word.

- `va_start` sets the list to the frame address plus the total size of the fixed arguments
  that were placed on the stack. That total is a compile-time constant, and it is zero in the
  common case where every fixed argument fits in a register, so `va_start` reduces to
  capturing the incoming `sp`.
- `va_arg` reads the object at the list's current address using the type's own load, then
  advances the list by the number of whole 8-byte slots that type occupies. For an aggregate
  passed by reference, the slot holds a pointer and `va_arg` yields the object that pointer
  names.
- `va_copy` copies the word.
- `va_end` does nothing and generates no instruction.

A function that captures the incoming `sp` for `va_start` does so before its prologue moves
`sp`, or computes the frame address back from `fp`. Both spellings are legal; the second is
what a frame-pointer function does naturally.

## The syscall convention

A system call enters the kernel with `sys`, which the instruction-inventory chapter defines
in two forms: `sys #imm` carries the syscall number as an 8-bit immediate, and `sys rs` takes
it from the low byte of a register. The number rides the instruction, not an argument
register, so all eight argument registers stay available to the call. Software that computes
a syscall number at run time places it in `t0` by convention.

Arguments travel in r2 through r9 exactly as they do for a function call, and the result
arrives in `a0`. Only word-sized scalars and pointers cross the boundary; the syscall
interface passes no aggregates by value, so the aggregate rules above never apply to it, and
a structure argument is always a pointer to caller-owned memory. The meaning of the returned
value, including how an error is encoded, belongs to the syscall-surface appendix rather than
to this one.

Register state across `sys` is stronger than across a `call`. Every general register except
`a0` and `a1` holds, on return to the interrupted instruction stream, the value it held when
`sys` executed. Both result registers are written because a two-word result returns its
second half in `a1`, and after a syscall whose result is one word the content of `a1` is
unspecified rather than preserved. The trap-model chapter's rule that syscall arguments stay live across the trap
boundary is what makes the kernel's side of this workable: the hardware frame saves no
general register, the kernel reads its arguments directly out of r2 through r9, and the
kernel saves and restores whichever registers its own code goes on to use. A kernel that
needs three temporaries saves three words, and a full register save happens only on an actual
context switch.

Three further properties fix the boundary.

- The user stack pointer is unchanged by `sys`, and the kernel runs on the stack its
  trap-stack control and status register names, so a syscall consumes no user stack and
  imposes no minimum free space below `sp`.
- The kernel does not read the user stack as part of the calling convention. An argument that
  does not fit in eight registers is passed as a pointer to a caller-owned buffer, which the
  kernel reads by ordinary translated access.
- The value in `a0` on return is the syscall's result, and a syscall that returns nothing
  still writes `a0` with a defined value rather than leaving the argument in place.

Preserving everything but the two result registers is a deliberate departure from the
function-call contract,
where the temporaries and the other argument registers are the caller's problem. The kernel
pays for it, and it pays little, because it saves only the registers it actually uses.
Software gains a syscall that can sit in the middle of a hot loop without a register-shuffling
prologue around it, and a translator gains a boundary it can model as a call that clobbers
two registers rather than as a wall.

## Worked examples

The examples below use the ABI register names and the assembler conventions the
instruction-inventory chapter states: lowercase mnemonics, no commas, source-to-destination
operand order, a mandatory base marker on every literal, and the `@` sigil on every memory
operand.

### A leaf call

The function below takes three `int` arguments and returns their sum. It calls nothing, so it
saves nothing, allocates no frame, and touches no memory.

    ; int add3(int a, int b, int c)
    add3:
        add.h a0 a1 a0        ; a plus b
        add.h a0 a2 a0        ; plus c
        return                ; result already in a0, zero-extended by add.h

The two `add.h` instructions leave the half-word result zero-extended in the full register,
which is the ABI form for an `int` result, so no extension instruction appears.

### A call that needs saved registers and spills

This function has to keep a value alive across a call, which no temporary can do, so it
claims one saved register and pays for a frame.

    ; long scale_and_add(long n) { return n + helper(n); }
    scale_and_add:
        subtract sp $20 sp    ; 32-byte frame, alignment preserved
        store ra @sp+$18      ; the link register, which the call will overwrite
        store s0 @sp+$10      ; the saved register this function borrows
        move a0 s0            ; keep n where the call cannot destroy it
        call helper           ; returns helper(n) in a0
        add a0 s0 a0          ; helper(n) plus n
        load @sp+$10 s0
        load @sp+$18 ra
        add sp $20 sp
        return

The frame holds two words of saved state and eight bytes of padding, because the frame size
is a multiple of 16. The function makes no call with stack arguments, so it allocates no
outgoing argument area, and `sp` is already the frame address at the `call`.

### A call returning a large structure

A 24-byte result exceeds two words, so it returns through caller-provided memory with a
hidden pointer in `a0` and the declared argument shifted into `a1`.

    ; struct triple { long a; long b; long c; };
    ; struct triple make_triple(long x)
    make_triple:
        store a1 @a0          ; result.a = x, through the hidden pointer
        add a1 $1 t0
        store t0 @a0+$8       ; result.b = x + 1
        add a1 $2 t0
        store t0 @a0+$10      ; result.c = x + 2
        return                ; a0 still holds the hidden pointer, as required

    ; struct triple t = make_triple(#3); use(t.c);
    call_make:
        subtract sp $20 sp    ; 24 bytes of result object plus the saved link register
        store ra @sp+$18
        move sp a0            ; hidden result pointer: the object begins at sp
        move.zb #3 a1         ; the one declared argument, shifted right by the hidden one
        call make_triple
        load @a0+$10 t0       ; read result.c through the pointer the callee returned
        load @sp+$18 ra
        add sp $20 sp
        return

`make_triple` is a leaf and saves nothing. A 16-byte result would need none of this
machinery: the callee would place the two words in `a0` and `a1` and return, and the caller
would keep them in registers.

### A variadic call and a variadic callee

The caller writes the whole variadic tail into the outgoing argument area at the bottom of
its frame, leaving the one fixed argument in `a0`.

    ; sum_ints(#2, x, y), where x is in t0 and y is in t1
    call_sum:
        subtract sp $20 sp    ; 16 bytes of variadic slots, saved link register, padding
        store ra @sp+$18
        store t0 @sp          ; first variadic slot, at the frame address the callee sees
        store t1 @sp+$8       ; second variadic slot
        move.zb #2 a0         ; the fixed argument, in a register as usual
        call sum_ints
        load @sp+$18 ra
        add sp $20 sp
        return

The callee captures the incoming `sp` as its `va_list` and walks it one slot at a time.

    ; unsigned sum_ints(unsigned count, ...)
    sum_ints:
        move sp t0            ; va_start: no fixed argument reached the stack
        move zero t1          ; the running total
    sum_loop:
        branch_eq a0 zero sum_done
        load.zh @t0 t2        ; va_arg for an int: one slot, zero-extended
        add.h t1 t2 t1
        add t0 $8 t0          ; advance the va_list by one slot
        subtract a0 $1 a0
        jump sum_loop
    sum_done:
        move t1 a0            ; the result, already in ABI form
        return

`sum_ints` allocates no frame and calls nothing, so its incoming `sp` remains the frame
address throughout and `va_start` costs one `move`. A frame-allocating variadic function
captures the same address either before it adjusts `sp` or by adding the frame size back.

## Conformance

The machine enforces none of this chapter, so a conformance binary cannot trap an ABI
violation the way it can trap a reserved opcode. What a test can do is check both sides of an
agreement, and the following properties are checkable that way.

- A caller and a callee compiled separately agree on the register or stack slot of every
  argument, for a corpus of signatures covering each classification rule, including the
  register-exhaustion and no-back-fill cases.
- A callee that clobbers a saved register is caught by a caller that places a known value in
  every saved register before the call and compares afterward.
- A function entered with `sp` a multiple of 16 leaves `sp` a multiple of 16 at every call it
  makes and returns with `sp` at the value it entered with.
- A variadic callee reading n arguments through `va_arg` reads exactly the values a caller
  placed in n slots, for each promoted type.
- Every general register except `a0` and `a1` holds its pre-`sys` value after a syscall
  returns, which a test pins by filling all 32 registers with distinct values and issuing a
  syscall with no side effect, ignoring `a1` because a one-word result leaves its content
  unspecified.

The last of those is the only property in this chapter a machine can fail on its own, since
the kernel is software and its register discipline is testable from user code without a
second compiler.
