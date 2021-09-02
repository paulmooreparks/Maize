# The Maize Virtual CPU 

This project implements a 64-bit virtual CPU called "Maize" on a library that enables the creation of virtual CPUs (called "Tortilla"). 

The near-term goal is to implement a set of devices to bridge from the virtual CPU environment to the host machine, create a "BIOS" layer 
above the virtual devices, implement a simple OS and a subset of Unix/Linux system calls (interrupt $80), 

## How To Use Maize

Maize is implemented in C++ and will run on Windows and Linux. I have not ported the assember (mazm) yet, but that will arrive as soon 
as I implement a few more instructions. For now, the binary just executes a few instructions that are loaded into memory from the main 
function.

## Project Status

As I said in the original [.NET implementation](https://github.com/paulmooreparks/Tortilla/), it's very early days for Maize, so don't 
expect too much in the way of application usability... yet! I'm still porting the basic text-mode console for input and output. 
Next, I'll start creating a file-system device. In the future I plan to port Clang or GCC to work with Maize binaries so that I can 
eventually port Linux to the virtual CPU.

In the short term, I'm implementing a very basic OS over a simple BIOS ([core.asm](https://github.com/paulmooreparks/Tortilla/blob/master/core.asm)). 
It will provide a basic character-mode [CLI](https://github.com/paulmooreparks/Tortilla/blob/master/cli.asm) to allow building and running simple Maize
programs from within the virtual CPU environment. 

So far, this implementation in C++ is MUCH faster and MUCH tighter than the .NET version.

## Hello, World!

Here is a simple "Hello, World!" application written in Maize assembly, targeting the basic OS I've written and the 
system calls it implements.

    INCLUDE "core.asm"
    INCLUDE "stdlib.asm"

    ; The CPU starts execution at segment $00000000, address $00001000, 
    ; so we'll put our code there.
    LABEL hw_start          $00001000

    ;******************************************************************************
    ; Entry point

    ; The AUTO parameter lets the assembler auto-calculate the address.
    LABEL hw_string         AUTO
    LABEL hw_string_end     AUTO

    hw_start:

       ; Set stack pointer. The back-tick (`)  is used as a number separator. 
       ; Underscore (_) and comma (,) may also be used as separators.
       LD $0000`2000 S.H0

       ; Set base pointer
       LD S.H0 S.H1

       ; If you don't want to bother setting the stack and base pointers, they're 
       ; already set to $FFFF`F000 by default, which should give you plenty of space.

       ; The basic ABI is for function arguments to be placed, from left to 
       ; right, into the G, H, J, K, L, and M registers. Any additional parameters 
       ; are pushed onto the stack.

       ; Get string length.
       LD hw_string G          ; Load the local pointer (current segment) to the string into G register
       CALL stdlib_strlen      ; Call stdlib function
       LD A J                  ; Return value (string length) is in A. Copy this to J for the write call

       ; Write string

       ; The kernel also implements a subset of Linux syscalls. 
       ; The syscall number is placed into the A register, and the first 
       ; six syscall arguments are placed, from left to right, into the 
       ; G, H, J, K, L, and M registers. Any remaining arguments are  
       ; pushed onto the stack. 

       LD G H.H0               ; Load the local pointer (current segment) to the string into H.H0 register
       CLR H.H1                ; We're running in segment zero
       LD $01 A                ; Load syscall opcode $01 (write) into A register
       LD $01 G                ; Load file descriptor $01 (STDOUT) into G register
       INT $80                 ; Call interrupt $80 to execute write syscall

       ; "Power down" the system, which actually means to exit the Maize CPU loop and return to the host OS.

       LD $A9 A                ; Load syscall opcode $A9 (169, sys_reboot) into A register
       LD $4321FEDC J          ; Load "magic" code for power down ($4321FEDC) into J register
       INT $80

    ;******************************************************************************
    ; This label points to the start of the string we want to output.

    hw_string: 
       STRING "Hello, world!\0"
