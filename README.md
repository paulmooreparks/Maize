# The Maize Virtual Machine

This project implements a 64-bit virtual machine called "Maize." This is an outgrowth of my [Tortilla](https://github.com/paulmooreparks/Tortilla) 
project, which began life as an x86 emulator implemented in C# on .NET and then later became a virtual CPU of my own making.

The near-term goal for the Maize project is to implement a set of devices to bridge from the virtual environment to the host machine, 
create a "BIOS" layer above the virtual devices, and implement a simple OS and a subset of Unix/Linux system calls (interrupt $80), 

## What It Is, Basically

* A 64-bit virtual machine implemented in C++ that executes a custom byte code
* An assembly language that represents the byte code
* An assembler implemented in C++ that generates byte code from the assembly language
* A very simple BIOS and OS that bridges the VM and the underlying machine
* An execution environment implemented in C++ that so far runs on Windows and Linux and could easily be ported to other platforms

## How To Use Maize

Maize is implemented in standard C++ and will run on Windows and Linux. The primary executable is [maize](https://github.com/paulmooreparks/Maize/blob/master/src/maize.cpp), 
which accepts a path to a binary file to execute. You may generate a binary from Maize assembly with the [mazm](https://github.com/paulmooreparks/Maize/blob/master/src/mazm.cpp) executable, 
which accepts a path to an assembly file.

I haven't finished porting all of the instructions from the .NET implementation yet, but now that I've finished restructuring the 
code it shouldn't take too long to complete them.

The [assembler](https://github.com/paulmooreparks/Maize/blob/master/src/mazm.cpp) is still VERY bare-bones, but it's enough to generate 
usable executables.

## Project Status

As I said in the original [.NET implementation](https://github.com/paulmooreparks/Tortilla/), it's very early days for Maize, so don't 
expect too much in the way of application usability... yet! I'm still porting the basic text-mode console for input and output. 
Next, I'll start creating a file-system device. I am currently porting [QBE](https://c9x.me/compile/) to output Maize assembly so that I can 
write Maize binaries with standard C and eventually port Linux to the virtual CPU.

In the short term, I'm implementing a very basic OS over a simple BIOS ([core.asm](https://github.com/paulmooreparks/Maize/blob/master/asm/core.asm)). 
It will provide a basic character-mode [CLI](https://github.com/paulmooreparks/Maize/blob/master/asm/cli.asm) to allow building and running simple 
Maize programs from within the virtual CPU environment. 

So far, this implementation in C++ is MUCH faster and MUCH tighter than the .NET version.

The near-term road map is as follows:

* Finish implementing all of the instructions documented below (in progress)
* Port [QBE](https://c9x.me/compile/) compiler backend to generate Maize byte code (in progress)
* Add ported QBE backend to [CProc](https://sr.ht/~mcf/cproc/) and build Maize BIOS & OS in standard C.
* Introduce "devices," which were partially implemented in the .NET version
* Implement a linker so that binaries can be built independently and linked together
* Clean up the assembler (mazm) and introduce some proper error checking
* Make the assembler read Unicode source files
* Implement floating-point arithmetic
* Add Maize backend to GCC and port GNU tools to Maize.

## Hello, World!

Here is a simple ["Hello, World!" application](https://github.com/paulmooreparks/Maize/blob/master/asm/hello.asm) 
written in Maize assembly.

    ; *****************************************************************************
    ; The entry point. Execution begins at segment $00000000, address $00000000

    $0000`0000:             ; The back-tick (`)  is used as a number separator. 
                            ; Underscore (_) and comma (,) may also be used as separators.
        CALL main
        HALT                ; For now, HALT exits the Maize interpreter, but in the
                            ; future it will pause the CPU and wait for an interrupt.

    ; *****************************************************************************
    ; The output message

    hw_string:
        STRING "Hello, world!\0"

    ; *****************************************************************************
    ; Return the length of a zero-terminated string. 
    ; A.H0: Address of string

    strlen:
        LD $0 G.H0          ; Set counter to zero.
    loop:
	    LEA G.H0 A.H0 H.H0  ; Add the counter to the address and put the result into H.H0.
        LD @H.H0 H.B4       ; Dereference H.H0 and copy character at that address into H.B4.
	    JZ loop_exit        ; LD sets the zero flag if the value copied to H.B4 is zero.
	    INC G.H0            ; Add 1 to the counter...
	    JMP loop            ; ...and continue the loop.
    loop_exit:
	    LD G.H0 A           ; Put the counter value into A, which is the return register.
	    RET                 ; Pop return address from stack and place into PC.

    ; *****************************************************************************
    ; The main function

    main:
        LD hw_string A.H0   ; Put address of message string into A.H0
        CALL strlen         ; Call strlen function to get the string length
        LD $01 G            ; $01 in G indicates output to stdout
        LD hw_string H.H0   ; H.H0 holds address of message to output
        LD A.H0 J           ; Put the string length into J
        SYS $01             ; Call output function implemented in Maize VM
        LD $0 A             ; Set return value for main
        RET                 ; Leave main

## Instruction Description 

Numeric values are represented in Maize assembly and Maize documentation in binary, decimal, or
hexadecimal formats. The % character precedes binary-encoded values, the # character precedes
decimal-encoded values, and the $ character precedes hexadecimal-encoded values.

    %00000001   binary value
    #123        decimal value
    $FFFE1000   hexadecimal value

The underscore, back-tick (`) and comma (,) characters may all be used as numeric separators in
all encodings.

Examples:

    %0000`0001
    %1001_1100
    #123,456,789
    $0000_FFFF
    $FE,DC,BA,98
    $1234`5678
    #123,456_789`021

Instructions are variable-length encoded and may have zero, one, or two parameters. An instruction
opcode is encoded as a single eight-bit byte defining the opcode and instruction flags, and each
instruction parameter is defined in an additional byte per parameter. Immediate values are encoded
following the opcode and parameter bytes in little-endian format (least-significant bytes are
stored lower in memory).

When an instruction has two parameters, the first parameter is the source parameter, and the second
parameter is the destination parameter.

Example: Load the value $01 into register A.

    LD $01 A

Example: Encoding of "LD $FFCC4411 D", which loads the immediate value $FFCC4411 into register D.
$41 is the opcode and flags for the instruction which loads an immediate value into a register.
$02 is the parameter byte specifying a four-byte immediate value as the source parameter. $3E is
the parameter byte specifying the 64-bit register D as the destination parameter. The bytes
following the parameters bytes are the immediate value in little-endian format.

    $41 $02 $3E $11 $44 $CC $FF

Immediate values may used as pointers into memory. This is represented in assembly by the '@'
prefix in front of the immediate value.

Example: Load the 64-bit value at address $0000`1000 into register B.

    LD @$0000`1000 B

Register values may be used as pointers into memory by adding a '@' prefix in front of the
register name.

Example: Store the value $FF into the byte pointed at by the A.H0 register:

    ST $FF @A.H0

Example: Load the quarter-word located at the address stored in D.H0 into sub-register Z.Q3:

    LD @D.H0 Z.Q3


## Registers

Registers are 64-bits wide (a "word") and are each divided into smaller sub-registers.

    A  General purpose
    B  General purpose
    C  General purpose
    D  General purpose
    E  General purpose
    G  General purpose
    H  General purpose
    J  General purpose
    K  General purpose
    L  General purpose
    M  General purpose
    Z  General purpose
    
    F  Flag register
    IN Instruction register
    P  Program execution register
    S  Stack register


### Sub-registers

Sub-registers are defined as half-word (H), quarter-word (Q), and byte (B) widths. The full
64-bit value of a register (for example, register A) may be coded as "A" or "A.W0". The register
value may also be accessed as separate half-word (32-bit) values, coded as A.H1 (upper 32 bits)
and A.H0 (lower 32-bits). The 16-bit quarter-words are similarly coded as A.Q3, A.Q2, A.Q1, and
A.Q0. Finally, the individual byte values are coded as A.B7, A.B6, A.B5, A.B4, A.B3, A.B2, A.B1,
and A.B0.

Shown graphically, the 64-bit value $FEDCBA9876543210 would be stored as follows:

     FE  DC  BA  98  76  54  32  10
    [B7][B6][B5][B4][B3][B2][B2][B0]
    [Q3    ][Q2    ][Q1    ][Q0    ]
    [H1            ][H0            ]
    [W0                            ]

In other words, if the following instruction were executed:

    LD $FEDCBA9876543210 A

The value stored in register A could then be represented as follows:

    A    = $FEDCBA9876543210
    A.W0 = $FEDCBA9876543210
    A.H1 = $FEDCBA98
    A.H0 = $76543210
    A.Q3 = $FEDC
    A.Q2 = $BA98
    A.Q1 = $7654
    A.Q0 = $3210
    A.B7 = $FE
    A.B6 = $DC
    A.B5 = $BA
    A.B4 = $98
    A.B3 = $76
    A.B2 = $54
    A.B1 = $32
    A.B0 = $10


### Special-purpose Registers

There are four special-purpose registers.

    F  Flags register. FL is an alias for F.H0, which contains a bit field of individual flags. 
       Flags in F.H1 may only be set in privileged mode.
 
    IN Instruction register, set by the instruction decoder as instructions and parameters are
       read from memory. This register can only be set by the decoder.
 
    P  Program execution register, which is the pointer to the next instruction to be decoded
       and executed. This is further sub-divided into P.H1, which is the current code segment,
       and P.H0, which is the effective program counter within the current segment. P.H1 may
       only be written to in privileged mode. PC is an alias for P.H0.
 
    S  Stack register, which is the location within the current segment at which the stack
       starts (growing downward in memory). This is further sub-divided into S.H1, which is
       the base pointer, and S.H0, which is the current stack pointer. SP is an alias for S.H0, 
       and BP is an alias for S.H1.

### Flags

(more coming on this)

## Execution

The CPU starts in privileged mode, and the program counter is initially set to segment $00000000,
address $00000000. When in privileged mode, the privilege flag is set, and instructions marked
as privileged may be executed. When the privilege flag is cleared, instruction execution and
memory access are limited to the current segment, and certain flags, registers, and instructions
are inaccessible. Program execution may return to privileged mode via hardware interrupts or via
software-generated (INT instruction) interrupts.


## Opcode Bytes

Opcodes are defined in an 8-bit byte separated into two flag bits and six opcode bits.

    %BBxx`xxxx  Flags bit field (bits 6 and 7)
    %xxBB`BBBB  Opcode bit field (bits 0 through 5)

When an instruction has a source parameter that may be either a register or an immediate value,
then bit 6 is interpreted as follows:

    %x0xx`xxxx  source parameter is a register
    %x1xx`xxxx  source parameter is an immediate value

When an instruction's source parameter may be either a value or a pointer to a value, then
bit 7 is interpreted as follows:

    %0xxx`xxxx  source parameter is a value
    %1xxx`xxxx  source parameter is a memory address

When bit 5 is set (%xx1x`xxxx), all eight bits are used to define the numeric opcode value.


## Instructions

    Binary      Hex   Mnemonic  Parameters      Description
    ----------  ---   --------  ----------      --------------------------------------------------------------------------------------------------------------------------------------
    %0000`0000  $00   HALT                      Halt the clock, thereby stopping execution (privileged)
    
    %0000`0001  $01   LD        regVal  reg     Load source register value into destination register
    %0100`0001  $41   LD        immVal  reg     Load immediate value into destination register
    %1000`0001  $81   LD        regAddr reg     Load value at address in source register into destination register
    %1100`0001  $C1   LD        immAddr reg     Load value at immediate address into destination register
    
    %0000`0010  $02   ST        regVal regAddr  Store source register value at address in second register
    %0100`0010  $42   ST        immVal regAddr  Store immediate value at address in destination register
    
    %0000`0011  $03   ADD       regVal  reg     Add source register value to destination register
    %0100`0011  $43   ADD       immVal  reg     Add immediate value to destination register
    %1000`0011  $83   ADD       regAddr reg     Add value at address in source register to destination register
    %1100`0011  $C3   ADD       immAddr reg     Add value at immediate address to destination register
    
    %0000`0100  $04   SUB       regVal  reg     Subtract source register value from destination register
    %0100`0100  $44   SUB       immVal  reg     Subtract immediate value from destination register
    %1000`0100  $84   SUB       regAddr reg     Subtract value at address in source register from destination register
    %1100`0100  $C4   SUB       immAddr reg     Subtract value at immediate address from destination register
    
    %0000`0101  $05   MUL       regVal  reg     Multiply destination register by source register value
    %0100`0101  $45   MUL       immVal  reg     Multiply destination register by immediate value
    %1000`0101  $85   MUL       regAddr reg     Multiply destination register by value at address in source register
    %1100`0101  $C5   MUL       immAddr reg     Multiply destination register by value at immediate address
    
    %0000`0110  $06   DIV       regVal  reg     Divide destination register by source register value
    %0100`0110  $46   DIV       immVal  reg     Divide destination register by immediate value
    %1000`0110  $86   DIV       regAddr reg     Divide destination register by value at address in source register
    %1100`0110  $C6   DIV       immAddr reg     Divide destination register by value at immediate address
    
    %0000`0111  $07   MOD       regVal  reg     Modulo destination register by source register value
    %0100`0111  $47   MOD       immVal  reg     Modulo destination register by immediate value
    %1000`0111  $87   MOD       regAddr reg     Modulo destination register by value at address in source register
    %1100`0111  $C7   MOD       immAddr reg     Modulo destination register by value at immediate address
    
    %0000`1000  $08   AND       regVal  reg     Bitwise AND destination register with source register value
    %0100`1000  $48   AND       immVal  reg     Bitwise AND destination register with immediate value
    %1000`1000  $88   AND       regAddr reg     Bitwise AND destination register with value at address in source register
    %1100`1000  $C8   AND       immAddr reg     Bitwise AND destination register with value at immediate address
    
    %0000`1001  $09   OR        regVal  reg     Bitwise OR destination register with source register value
    %0100`1001  $49   OR        immVal  reg     Bitwise OR destination register with immediate value
    %1000`1001  $89   OR        regAddr reg     Bitwise OR destination register with value at address in source register
    %1100`1001  $C9   OR        immAddr reg     Bitwise OR destination register with value at immediate address
    
    %0000`1010  $0A   NOR       regVal  reg     Bitwise NOR destination register with source register value
    %0100`1010  $4A   NOR       immVal  reg     Bitwise NOR destination register with immediate value
    %1000`1010  $8A   NOR       regAddr reg     Bitwise NOR destination register with value at address in source register
    %1100`1010  $CA   NOR       immAddr reg     Bitwise NOR destination register with value at immediate address
    
    %0000`1011  $0B   NAND      regVal  reg     Bitwise NAND destination register with source register value
    %0100`1011  $4B   NAND      immVal  reg     Bitwise NAND destination register with immediate value
    %1000`1011  $8B   NAND      regAddr reg     Bitwise NAND destination register with value at address in source register
    %1100`1011  $CB   NAND      immAddr reg     Bitwise NAND destination register with value at immediate address
    
    %0000`1100  $0C   XOR       regVal  reg     Bitwise XOR destination register with source register value
    %0100`1100  $4C   XOR       immVal  reg     Bitwise XOR destination register with immediate value
    %1000`1100  $8C   XOR       regAddr reg     Bitwise XOR destination register with value at address in source register
    %1100`1100  $CC   XOR       immAddr reg     Bitwise XOR destination register with value at immediate address
    
    %0000`1101  $0D   SHL       regVal  reg     Shift value in destination register left by value in source register
    %0100`1101  $4D   SHL       immVal  reg     Shift value in destination register left by immediate value
    %1000`1101  $8D   SHL       regAddr reg     Shift value in destination register left by value at address in source register
    %1100`1101  $CD   SHL       immAddr reg     Shift value in destination register left by value at immediate address
    
    %0000`1110  $0E   SHR       regVal  reg     Shift value in destination register right by value in source register
    %0100`1110  $4E   SHR       immVal  reg     Shift value in destination register right by immediate value
    %1000`1110  $8E   SHR       regAddr reg     Shift value in destination register right by value at address in source register
    %1100`1110  $CE   SHR       immAddr reg     Shift value in destination register right by value at immediate address
    
    %0000`1111  $0F   CMP       regVal  reg     Set flags by subtracting source register value from destination register
    %0100`1111  $4F   CMP       immVal  reg     Set flags by subtracting immediate value from destination register
    %1000`1111  $8F   CMP       regAddr reg     Set flags by subtracting value at address in source register from destination register
    %1100`1111  $CF   CMP       immAddr reg     Set flags by subtracting value at immediate address from destination register
    
    %0001`0000  $10   TEST      regVal  reg     Set flags by ANDing source register value with destination register
    %0101`0000  $50   TEST      immVal  reg     Set flags by ANDing immediate value with destination register
    %1001`0000  $90   TEST      regAddr reg     Set flags by ANDing value at address in source register with destination register
    %1101`0000  $D0   TEST      immAddr reg     Set flags by ANDing value at immediate address with destination register
    
    %0001`0001  $11   CMPXCHG   regVal  reg reg Compare value in operand 2 with value in operand 3. If equal, set zero flag and load value in operand 1 into operand 2. Otherwise, clear zero flag and load value in operand 2 into operand 3.
    %0101`0001  $51   CMPXCHG   immVal  reg reg reserved
    %1001`0001  $91   CMPXCHG   regAddr reg reg reserved
    %1101`0001  $D1   CMPXCHG   immAddr reg reg reserved
    
    %0001`0010  $12   LEA       regVal  reg reg Add register value in operand 1 to value in operand 2 register and store result in operand 3 register
    %0101`0010  $52   LEA       immVal  reg reg Add immediate value in operand 1 to value in operand 2 register and store result in operand 3 register
    %1001`0010  $92   LEA       regAddr reg reg Add value at address in operand 1 register to value in operand 2 register and store result in operand 3 register
    %1101`0010  $D2   LEA       immAddr reg reg Add value at immediate address in operand 1 to value in operand 2 register and store result in operand 3 register
    
    %0001`0011  $13   LDX       regVal  reg     Load source register value into destination register with zero extension
    %0101`0011  $53   LDX       immVal  reg     Load immediate value into destination register with zero extension
    %1001`0011  $93   LDX       regAddr reg     Load value at address in source register into destination register with zero extension
    %1101`0011  $D3   LDX       immAddr reg     Load value at immediate address into destination register with zero extension
    
    %0001`0100  $14   OUT       regVal  imm     Output value in source register to destination port
    %0101`0100  $54   OUT       immVal  imm     Output immediate value to destination port
    %1001`0100  $94   OUT       regAddr imm     Output value at address in source register to destination port
    %1101`0100  $D4   OUT       immAddr imm     Output value at immediate address to destination port
    
    %0001`0101  $15   LNGJMP    regVal          Jump to segment and address in source register and continue execution (privileged)
    %0101`0101  $55   LNGJMP    immVal          Jump to immediate segment and address and continue execution (privileged)
    %1001`0101  $95   LNGJMP    regAddr         Jump to segment and address pointed to by source register and continue execution (privileged)
    %1101`0101  $D5   LNGJMP    immAddr         Jump to segment and address pointed to by immediate value and continue execution (privileged)
    
    %0001`0110  $16   JMP       regVal          Jump to address in source register and continue execution
    %0101`0110  $56   JMP       immVal          Jump to immediate address and continue execution
    %1001`0110  $96   JMP       regAddr         Jump to address pointed to by source register and continue execution
    %1101`0110  $D6   JMP       immAddr         Jump to address pointed to by immediate value and continue execution
    
    %0001`0111  $17   JZ        regVal          If Zero flag is set, jump to address in source register and continue execution
    %0101`0111  $57   JZ        immVal          If Zero flag is set, jump to immediate address and continue execution
    %1001`0111  $97   JZ        regAddr         If Zero flag is set, jump to address pointed to by source register and continue execution
    %1101`0111  $D7   JZ        immAddr         If Zero flag is set, jump to address pointed to by immediate value and continue execution
    
    %0001`1000  $18   JNZ       regVal          If Zero flag is not set, jump to address in source register and continue execution
    %0101`1000  $58   JNZ       immVal          If Zero flag is not set, jump to immediate address and continue execution
    %1001`1000  $98   JNZ       regAddr         If Zero flag is not set, jump to address pointed to by source register and continue execution
    %1101`1000  $D8   JNZ       immAddr         If Zero flag is not set, jump to address pointed to by immediate value and continue execution
    
    %0001`1001  $19   JLT       regVal          If Negative flag is not equal to Overflow flag, jump to address in source register and continue execution
    %0101`1001  $59   JLT       immVal          If Negative flag is not equal to Overflow flag, jump to immediate address and continue execution
    %1001`1001  $99   JLT       regAddr         If Negative flag is not equal to Overflow flag, jump to address pointed to by source register and continue execution
    %1101`1001  $D9   JLT       immAddr         If Negative flag is not equal to Overflow flag, jump to address pointed to by immediate value and continue execution
    
    %0001`1010  $1A   JB        reg             If Carry flag is set, jump to address in source register and continue execution
    %0101`1010  $5A   JB        imm             If Carry flag is set, jump to immediate address and continue execution
    %1001`1010  $9A   JB        regAddr         If Carry flag is set, jump to address pointed to by source register and continue execution
    %1101`1010  $DA   JB        immAddr         If Carry flag is set, jump to address pointed to by immediate value and continue execution
    
    %0001`1011  $1B   JGT       regVal          If Zero flag is clear and Negative flag is not equal to Overflow flag, jump to address in source register and continue execution
    %0101`1011  $5B   JGT       immVal          If Zero flag is clear and Negative flag is not equal to Overflow flag, jump to immediate address and continue execution
    %1001`1011  $9B   JGT       regAddr         If Zero flag is clear and Negative flag is not equal to Overflow flag, jump to address pointed to by source register and continue execution
    %1101`1011  $DB   JGT       immAddr         If Zero flag is clear and Negative flag is not equal to Overflow flag, jump to address pointed to by immediate value and continue execution
    
    %0001`1100  $1C   JA        regVal          If Carry flag is clear and Zero flag is clear, jump to address in source register and continue execution
    %0101`1100  $5C   JA        immVal          If Carry flag is clear and Zero flag is clear, jump to immediate address and continue execution
    %1001`1100  $9C   JA        regAddr         If Carry flag is clear and Zero flag is clear, jump to address pointed to by source register and continue execution
    %1101`1100  $DC   JA        immAddr         If Carry flag is clear and Zero flag is clear, jump to address pointed to by immediate value and continue execution
    
    %0001`1101  $1D   CALL      regVal          Push PC.H0 to stack, jump to address in source register and continue execution until RET is executed
    %0101`1101  $5D   CALL      immVal          Push PC.H0 to stack, jump to immediate address and continue execution until RET is executed
    %1001`1101  $9D   CALL      regAddr         Push PC.H0 to stack, jump to address pointed to by source register and continue execution until RET is executed
    %1101`1101  $DD   CALL      immAddr         Push PC.H0 to stack, jump to address pointed to by immediate value and continue execution until RET is executed
    
    %0001`1110  $1E   OUTR      regVal  reg     Output value in source register to port in destination register
    %0101`1110  $5E   OUTR      immVal  reg     Output immediate value to port in destination register
    %1001`1110  $9E   OUTR      regAddr reg     Output value at address in source register to port in destination register
    %1101`1110  $DE   OUTR      immAddr reg     Output value at immediate address to port in destination register
    
    %0001`1111  $1F   IN        regVal  reg     Read value from port in source register into destination register
    %0101`1111  $5F   IN        immVal  reg     Read value from port in immediate value into destination register
    %1001`1111  $9F   IN        regAddr reg     Read value from port at address in source register into destination register
    %1101`1111  $DF   IN        immAddr reg     Read value from port at immediate address into destination register
    
    %0010`0000  $20   PUSH      regVal          Copy register value into memory at location in S.H0, decrement S.H0 by size of register
    %0110`0000  $60   PUSH      immVal          Copy immediate value into memory at location in S.H0, decrement S.H0 by size of immediate value
    
    %0010`0010  $22   CLR       regVal          Set register to zero (0).
    
    %0010`0100  $24   INT       regVal          Push FL and PC to stack and generate a software interrupt at index stored in register (privileged)
    %0110`0100  $64   INT       immVal          Push FL and PC to stack and generate a software interrupt using immediate index (privileged)
    
    %0010`0110  $26   POP       regVal          Increment SP.H0 by size of register, copy value at SP.H0 into register
    
    %0010`0111  $27   RET                       Pop PC.H0 from stack and continue execution at that address. Used to return from CALL.
    
    %0010`1000  $28   IRET                      Pop FL and PC from stack and continue execution at segment/address in PC. Used to return from interrupt (privileged).
    
    %0010`1001  $29   SETINT                    Set the Interrupt flag, thereby enabling hardware interrupts (privileged)
    
    %0010`1111  $2F   CMPIND    regVal regAddr  Set flags by subtracting source register value from value at address in destination register
    %0110`1111  $6F   CMPIND    immVal regAddr  Set flags by subtracting immediate value from value at address in destination register
    
    %0011`0000  $30   TSTIND    regVal regAddr  Set flags by ANDing source register value with value at address in destination register
    %0111`0000  $70   TSTIND    immVal regAddr  Set flags by ANDing immediate value with value at address in destination register
    
    %0011`0001  $31   INC       regVal          Increment register by 1.

    %0011`0010  $32   DEC       regVal          Decrement register by 1.

    %0011`0011  $33   NOT       regVal          Bitwise negate value in register, store result in register.

    %0011`0100  $34   SYS       regVal          Execute a system call using the system-call index stored in register (privileged)
    %0111`0100  $74   SYS       immVal          Execute a system call using the immediate index (privileged)
    
    %1010`1010  $AA   NOP                       No operation. Used as an instruction placeholder.
    
    %1110`0000  $E0   XCHG      reg     reg     Atomically exchange the values in two registers
    
    %1110`0001  $E1   SETCRY                    Set the Carry flag
    
    %1110`0010  $E2   CLRCRY                    Clear the Carry flag
    
    %1110`0011  $E3   CLRINT                    Clear the Interrupt flag, thereby disabling hardware interrupts (privileged)
    
    %1110`0100  $E4   DUP                       Duplicate the top value on the stack

    %1110`0101  $E5   SWAP                      Swap the top two values on the stack

    %1111`1111  $FF   BRK                       Trigger a debug break


## Register Parameter

### Register bit field

    %0000xxxx   $0    A register
    %0001xxxx   $1    B register
    %0010xxxx   $2    C register
    %0011xxxx   $3    D register
    %0100xxxx   $4    E register
    %0101xxxx   $5    G register
    %0110xxxx   $6    H register
    %0111xxxx   $7    J register
    %1000xxxx   $8    K register
    %1001xxxx   $9    L register
    %1010xxxx   $A    M register
    %1011xxxx   $B    Z register
    %1100xxxx   $C    F register (flags)
    %1101xxxx   $D    IN register (instruction)
    %1110xxxx   $E    P register (program segment / counter)
    %1111xxxx   $F    S register (stack pointers)

### Sub-register bit field

    %xxxx0000   $0   X.B0 (1-byte data)
    %xxxx0001   $1   X.B1 (1-byte data)
    %xxxx0010   $2   X.B2 (1-byte data)
    %xxxx0011   $3   X.B3 (1-byte data)
    %xxxx0100   $4   X.B4 (1-byte data)
    %xxxx0101   $5   X.B5 (1-byte data)
    %xxxx0110   $6   X.B6 (1-byte data)
    %xxxx0111   $7   X.B7 (1-byte data)
    %xxxx1000   $8   X.Q0 (2-byte data)
    %xxxx1001   $9   X.Q1 (2-byte data)
    %xxxx1010   $A   X.Q2 (2-byte data)
    %xxxx1011   $B   X.Q3 (2-byte data)
    %xxxx1100   $C   X.H0 (4-byte data)
    %xxxx1101   $D   X.H1 (4-byte data)
    %xxxx1110   $E   X    (8-byte data)


## Immediate Parameter

### Immediate Operation Bit

    %xxxx`0xxx       Read immediate value as operand
    %xxxx`1xxx       Perform math operation with value (not implemented yet)

### Immediate Value Bit Field

    %xxxx`x000  $x0  instruction reads 1 byte immediate (8 bits)
    %xxxx`x001  $x1  instruction reads 2-byte immediate (16 bits)
    %xxxx`x010  $x2  instruction reads 4-byte immediate (32 bits)
    %xxxx`x011  $x3  instruction reads 8-byte immediate (64 bits)

### Immediate Math Bit Field

    %0000`xxxx  $0x  ADD immediate to previous operand
    %0001`xxxx  $1x  SUB immediate from previous operand
    %0010`xxxx  $2x  MUL previous operand by immediate
    %0011`xxxx  $3x  DIV previous operand by immediate
    %0100`xxxx  $4x  AND previous operand with immediate
    %0101`xxxx  $5x  OR previous operand with immediate
    %0110`xxxx  $6x  XOR previous operand with immediate
    %0111`xxxx  $7x  NOR previous operand with immediate
    %1000`xxxx  $8x  NAND previous operand with immediate
    %1001`xxxx  $9x  SHL previous operand by immediate
    %1010`xxxx  $Ax  SHR previous operand by immediate
    %1011`xxxx  $Bx  reserved
    %1100`xxxx  $Cx  reserved
    %1101`xxxx  $Dx  reserved
    %1110`xxxx  $Ex  reserved
    %1111`xxxx  $Fx  reserved


## BIOS Interface

BIOS calls will track as closely as possible to the "standard" BIOS routines found on typical
x86 PCs. The x86 registers used in BIOS calls will map to Maize registers as follows:

    AX -> A.Q0
       AL -> A.B0
       AH -> A.B1
    BX -> A.Q1
       BL -> A.B2
       BH -> A.B3
    CX -> A.Q2
       CL -> A.B4
       CH -> A.B5
    DX -> A.Q3
       DL -> A.B6
       DH -> A.B7


## OS ABI

The first six arguments to OS-level routines will be placed, from left to right, into the
G, H, J, K, L, and M registers. Any remaining arguments will be pushed onto the stack.
For example:

    ; Call C function "void random_os_function(int32_t a, const char *b, size_t c, int32_t* d)"
    LD $0000`1234 G         ; int32_t a
    LD A.H0 H               ; const char* b, assuming the pointer is in A.H0
    LD $0000`00FF J         ; size_t c
    LD B.H1 K               ; int32_t* d, assuming the pointer is in B.H1
    CALL random_os_function

The called routine will preserve B, C, D, E, Z, and SP. Any other registers may not be
preserved. Return values will placed into the A register. For example:

    ; Implement C function "int add(int a, int b)"
    LD G A
    ADD H A
    RET

For syscalls, the same standard will be followed for syscall parameters. The syscall number
will be placed into the A register prior to calling the interrupt.

    ; Output a string using sys_write
    LD $01 A             ; syscall 1 = sys_write
    LD $01 G             ; file descriptor 1 (STDOUT) in register G
    LD hello_world H.H0  ; string address in register H
    LD hello_world_end J
    SUB hello_world J    ; string length in register J
    INT $80              ; call sys_write

The same syscall may be made with the SYS instruction, which will execute the syscall directly.

    ; Output a string using sys_write, calling directly via SYS instruction
    LD $01 G             ; file descriptor 1 (STDOUT) in register G
    LD hello_world H.H0  ; string address in register H
    LD hello_world_end J
    SUB hello_world J    ; string length in register J
    SYS $01              ; call sys_write


## Assembler Syntax

(This section is incomplete and a bit of a work in progress. Refer to HelloWorld.asm,
stdlib.asm, and core.asm for practical examples.)

Tokens with leading double-underscore (e.g., __foo) are reserved.

    %00000001   binary
    #123        decimal
    $FFFE1000   hexadecimal

Other syntax, to be described more fully later:

    LABEL labelName labelData | AUTO

    DATA dataValue [dataValue] [dataValue] [...]

    STRING "stringvalue"

    ADDRESS address | labelName


## Opcodes Sorted Numerically

    Binary      Hex   Mnemonic  Parameters      Description
    ----------  ---   --------  ----------      --------------------------------------------------------------------------------------------------------------------------------------
    %0000`0000  $00   HALT                      Halt the clock, thereby stopping execution (privileged)
    %0000`0001  $01   LD        regVal  reg     Load source register value into destination register
    %0000`0010  $02   ST        regVal regAddr  Store source register value at address in second register
    %0000`0011  $03   ADD       regVal  reg     Add source register value to destination register
    %0000`0100  $04   SUB       regVal  reg     Subtract source register value from destination register
    %0000`0101  $05   MUL       regVal  reg     Multiply destination register by source register value
    %0000`0110  $06   DIV       regVal  reg     Divide destination register by source register value
    %0000`0111  $07   MOD       regVal  reg     Modulo destination register by source register value
    %0000`1000  $08   AND       regVal  reg     Bitwise AND destination register with source register value
    %0000`1001  $09   OR        regVal  reg     Bitwise OR destination register with source register value
    %0000`1010  $0A   NOR       regVal  reg     Bitwise NOR destination register with source register value
    %0000`1011  $0B   NAND      regVal  reg     Bitwise NAND destination register with source register value
    %0000`1100  $0C   XOR       regVal  reg     Bitwise XOR destination register with source register value
    %0000`1101  $0D   SHL       regVal  reg     Shift value in destination register left by value in source register
    %0000`1110  $0E   SHR       regVal  reg     Shift value in destination register right by value in source register
    %0000`1111  $0F   CMP       regVal  reg     Set flags by subtracting source register value from destination register
    %0001`0000  $10   TEST      regVal  reg     Set flags by ANDing source register value with destination register
    %0001`0001  $11   CMPXCHG   regVal  reg     reserved
    %0001`0010  $12   LEA       regVal  reg reg 
    %0001`0011  $13   LDX       regVal  reg     Load source register value into destination register with sign extension
    %0001`0100  $14   OUT       regVal  imm     Output value in source register to destination port
    %0001`0101  $15   LNGJMP    regVal          Jump to segment and address in source register and continue execution (privileged)
    %0001`0110  $16   JMP       regVal          Jump to address in source register and continue execution
    %0001`0111  $17   JZ        regVal          If Zero flag is set, jump to address in source register and continue execution
    %0001`1000  $18   JNZ       regVal          If Zero flag is not set, jump to address in source register and continue execution
    %0001`1001  $19   JLT       regVal          If Negative flag is not equal to Overflow flag, jump to address in source register and continue execution
    %0001`1010  $1A   JB        reg             If Carry flag is set, jump to address in source register and continue execution
    %0001`1011  $1B   JGT       regVal          If Zero flag is clear and Negative flag is not equal to Overflow flag, jump to address in source register and continue execution
    %0001`1100  $1C   JA        regVal          If Carry flag is clear and Zero flag is clear, jump to address in source register and continue execution
    %0001`1101  $1D   CALL      regVal          Push PC.H0 to stack, jump to address in source register and continue execution until RET is executed
    %0001`1110  $1E   OUTR      regVal  reg     Output value in source register to port in destination register
    %0001`1111  $1F   IN        regVal  reg     Read value from port in source register into destination register
    %0010`0000  $20   PUSH      regVal          Copy register value into memory at location in S.H0, decrement S.H0 by size of register
    %0010`0010  $22   CLR       regVal          Set register to zero (0).
    %0010`0100  $24   INT       regVal          Push FL and PC to stack and generate a software interrupt at index stored in register (privileged)
    %0010`0110  $26   POP       regVal          Increment SP.H0 by size of register, copy value at SP.H0 into register
    %0010`0111  $27   RET                       Pop PC.H0 from stack and continue execution at that address. Used to return from CALL.
    %0010`1000  $28   IRET                      Pop FL and PC from stack and continue execution at segment/address in PC. Used to return from interrupt (privileged).
    %0010`1001  $29   SETINT                    Set the Interrupt flag, thereby enabling hardware interrupts (privileged)
    %0010`1111  $2F   CMPIND    regVal regAddr  Set flags by subtracting source register value from value at address in destination register
    %0011`0000  $30   TSTIND    regVal regAddr  Set flags by ANDing source register value with value at address in destination register
    %0011`0001  $31   INC       regVal          Increment register by 1.
    %0011`0010  $32   DEC       regVal          Decrement register by 1.
    %0011`0011  $33   NOT       regVal          Bitwise negate value in register, store result in register.
    %0011`0100  $34   SYS       regVal          Execute a system call using the system-call index stored in register (privileged)
    %0100`0001  $41   LD        immVal  reg     Load immediate value into destination register
    %0100`0010  $42   ST        immVal regAddr  Store immediate value at address in destination register
    %0100`0011  $43   ADD       immVal  reg     Add immediate value to destination register
    %0100`0100  $44   SUB       immVal  reg     Subtract immediate value from destination register
    %0100`0101  $45   MUL       immVal  reg     Multiply destination register by immediate value
    %0100`0110  $46   DIV       immVal  reg     Divide destination register by immediate value
    %0100`0111  $47   MOD       immVal  reg     Modulo destination register by immediate value
    %0100`1000  $48   AND       immVal  reg     Bitwise AND destination register with immediate value
    %0100`1001  $49   OR        immVal  reg     Bitwise OR destination register with immediate value
    %0100`1010  $4A   NOR       immVal  reg     Bitwise NOR destination register with immediate value
    %0100`1011  $4B   NAND      immVal  reg     Bitwise NAND destination register with immediate value
    %0100`1100  $4C   XOR       immVal  reg     Bitwise XOR destination register with immediate value
    %0100`1101  $4D   SHL       immVal  reg     Shift value in destination register left by immediate value
    %0100`1110  $4E   SHR       immVal  reg     Shift value in destination register right by immediate value
    %0100`1111  $4F   CMP       immVal  reg     Set flags by subtracting immediate value from destination register
    %0101`0000  $50   TEST      immVal  reg     Set flags by ANDing immediate value with destination register
    %0101`0001  $51   CMPXCHG   immVal  reg     TODO
    %0101`0010  $52   LEA       immVal  reg reg TODO
    %0101`0011  $53   LDX       immVal  reg     Load immediate value into destination register with sign extension
    %0101`0100  $54   OUT       immVal  imm     Output immediate value to destination port
    %0101`0101  $55   LNGJMP    immVal          Jump to immediate segment and address and continue execution (privileged)
    %0101`0110  $56   JMP       immVal          Jump to immediate address and continue execution
    %0101`0111  $57   JZ        immVal          If Zero flag is set, jump to immediate address and continue execution
    %0101`1000  $58   JNZ       immVal          If Zero flag is not set, jump to immediate address and continue execution
    %0101`1001  $59   JLT       immVal          If Negative flag is not equal to Overflow flag, jump to immediate address and continue execution
    %0101`1010  $5A   JB        imm             If Carry flag is set, jump to immediate address and continue execution
    %0101`1011  $5B   JGT       immVal          If Zero flag is clear and Negative flag is not equal to Overflow flag, jump to immediate address and continue execution
    %0101`1100  $5C   JA        immVal          If Carry flag is clear and Zero flag is clear, jump to immediate address and continue execution
    %0101`1101  $5D   CALL      immVal          Push PC.H0 to stack, jump to immediate address and continue execution until RET is executed
    %0101`1110  $5E   OUTR      immVal  reg     Output immediate value to port in destination register
    %0101`1111  $5F   IN        immVal  reg     Read value from port in immediate value into destination register
    %0110`0000  $60   PUSH      immVal          Copy immediate value into memory at location in S.H0, decrement S.H0 by size of immediate value
    %0110`0100  $64   INT       immVal          Push FL and PC to stack and generate a software interrupt using immediate index (privileged)
    %0110`1111  $6F   CMPIND    immVal regAddr  Set flags by subtracting immediate value from value at address in destination register
    %0111`0000  $70   TSTIND    immVal regAddr  Set flags by ANDing immediate value with value at address in destination register
    %0111`0100  $74   SYS       immVal          Execute a system call using the immediate index (privileged)
    %1000`0001  $81   LD        regAddr reg     Load value at address in source register into destination register
    %1000`0011  $83   ADD       regAddr reg     Add value at address in source register to destination register
    %1010`0100  $84   SUB       regAddr reg     Subtract value at address in source register from destination register
    %1000`0101  $85   MUL       regAddr reg     Multiply destination register by value at address in source register
    %1000`0110  $86   DIV       regAddr reg     Divide destination register by value at address in source register
    %1000`0111  $87   MOD       regAddr reg     Modulo destination register by value at address in source register
    %1000`1000  $88   AND       regAddr reg     Bitwise AND destination register with value at address in source register
    %1000`1001  $89   OR        regAddr reg     Bitwise OR destination register with value at address in source register
    %1000`1010  $8A   NOR       regAddr reg     Bitwise NOR destination register with value at address in source register
    %1000`1011  $8B   NAND      regAddr reg     Bitwise NAND destination register with value at address in source register
    %1000`1100  $8C   XOR       regAddr reg     Bitwise XOR destination register with value at address in source register
    %1000`1101  $8D   SHL       regAddr reg     Shift value in destination register left by value at address in source register
    %1000`1110  $8E   SHR       regAddr reg     Shift value in destination register right by value at address in source register
    %1000`1111  $8F   CMP       regAddr reg     Set flags by subtracting value at address in source register from destination register
    %1001`0000  $90   TEST      regAddr reg     Set flags by ANDing value at address in source register with destination register
    %1001`0001  $91   CMPXCHG   regAddr reg     reserved
    %1001`0010  $92   LEA       regAddr reg reg reserved
    %1001`0011  $93   LDX       regAddr reg     Load value at address in source register into destination register with sign extension
    %1001`0100  $94   OUT       regAddr imm     Output value at address in source register to destination port
    %1001`0101  $95   LNGJMP    regAddr         Jump to segment and address pointed to by source register and continue execution (privileged)
    %1001`0110  $96   JMP       regAddr         Jump to address pointed to by source register and continue execution
    %1001`0111  $97   JZ        regAddr         If Zero flag is set, jump to address pointed to by source register and continue execution
    %1001`1000  $98   JNZ       regAddr         If Zero flag is not set, jump to address pointed to by source register and continue execution
    %1001`1001  $99   JLT       regAddr         If Negative flag is not equal to Overflow flag, jump to address pointed to by source register and continue execution
    %1001`1010  $9A   JB        regAddr         If Carry flag is set, jump to address pointed to by source register and continue execution
    %1001`1011  $9B   JGT       regAddr         If Zero flag is clear and Negative flag is not equal to Overflow flag, jump to address pointed to by source register and continue execution
    %1001`1100  $9C   JA        regAddr         If Carry flag is clear and Zero flag is clear, jump to address pointed to by source register and continue execution
    %1001`1101  $9D   CALL      regAddr         Push PC.H0 to stack, jump to address pointed to by source register and continue execution until RET is executed
    %1001`1110  $9E   OUTR      regAddr reg     Output value at address in source register to port in destination register
    %1001`1111  $9F   IN        regAddr reg     Read value from port at address in source register into destination register
    %1010`0100  $84   SUB       regAddr reg     Subtract value at address in source register from destination register
    %1010`1010  $AA   NOP                       No operation. Used as an instruction placeholder.
    %1100`0001  $C1   LD        immAddr reg     Load value at immediate address into destination register
    %1100`0011  $C3   ADD       immAddr reg     Add value at immediate address to destination register
    %1100`0100  $C4   SUB       immAddr reg     Subtract value at immediate address from destination register
    %1100`0101  $C5   MUL       immAddr reg     Multiply destination register by value at immediate address
    %1100`0110  $C6   DIV       immAddr reg     Divide destination register by value at immediate address
    %1100`0111  $C7   MOD       immAddr reg     Modulo destination register by value at immediate address
    %1100`1000  $C8   AND       immAddr reg     Bitwise AND destination register with value at immediate address
    %1100`1001  $C9   OR        immAddr reg     Bitwise OR destination register with value at immediate address
    %1100`1010  $CA   NOR       immAddr reg     Bitwise NOR destination register with value at immediate address
    %1100`1011  $CB   NAND      immAddr reg     Bitwise NAND destination register with value at immediate address
    %1100`1100  $CC   XOR       immAddr reg     Bitwise XOR destination register with value at immediate address
    %1100`1101  $CD   SHL       immAddr reg     Shift value in destination register left by value at immediate address
    %1100`1110  $CE   SHR       immAddr reg     Shift value in destination register right by value at immediate address
    %1100`1111  $CF   CMP       immAddr reg     Set flags by subtracting value at immediate address from destination register
    %1101`0000  $D0   TEST      immAddr reg     Set flags by ANDing value at immediate address with destination register
    %1101`0001  $D1   CMPXCHG   immAddr reg     reserved
    %1101`0010  $D2   LEA       immAddr reg reg reserved
    %1101`0011  $D3   LDX       immAddr reg     Load value at immediate address into destination register with sign extension
    %1101`0100  $D4   OUT       immAddr imm     Output value at immediate address to destination port
    %1101`0101  $D5   LNGJMP    immAddr         Jump to segment and address pointed to by immediate value and continue execution (privileged)
    %1101`0110  $D6   JMP       immAddr         Jump to address pointed to by immediate value and continue execution
    %1101`0111  $D7   JZ        immAddr         If Zero flag is set, jump to address pointed to by immediate value and continue execution
    %1101`1000  $D8   JNZ       immAddr         If Zero flag is not set, jump to address pointed to by immediate value and continue execution
    %1101`1001  $D9   JLT       immAddr         If Negative flag is not equal to Overflow flag, jump to address pointed to by immediate value and continue execution
    %1101`1010  $DA   JB        immAddr         If Carry flag is set, jump to address pointed to by immediate value and continue execution
    %1101`1011  $DB   JGT       immAddr         If Zero flag is clear and Negative flag is not equal to Overflow flag, jump to address pointed to by immediate value and continue execution
    %1101`1100  $DC   JA        immAddr         If Carry flag is clear and Zero flag is clear, jump to address pointed to by immediate value and continue execution
    %1101`1101  $DD   CALL      immAddr         Push PC.H0 to stack, jump to address pointed to by immediate value and continue execution until RET is executed
    %1101`1110  $DE   OUTR      immAddr reg     Output value at immediate address to port in destination register
    %1101`1111  $DF   IN        immAddr reg     Read value from port at immediate address into destination register
    %1110`0000  $E0   XCHG      reg     reg     Atomically exchange the values in two registers
    %1110`0001  $E1   SETCRY                    Set the Carry flag
    %1110`0010  $E2   CLRCRY                    Clear the Carry flag
    %1110`0011  $E3   CLRINT                    Clear the Interrupt flag, thereby disabling hardware interrupts (privileged)
    %1110`0100  $E4   DUP                       Duplicate the top value on the stack
    %1110`0101  $E5   SWAP                      Swap the top two values on the stack
    %1111`1111  $FF   BRK                       Trigger a debug break
