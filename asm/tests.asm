INCLUDE "core.asm"
INCLUDE "stdlib.asm"

; The CPU starts execution at segment $00000000, address $00001000, 
; so we'll put our code there.
LABEL hw_start            $00001000   

; The AUTO parameter lets the assembler auto-calculate the address.
LABEL hw_string         AUTO
LABEL hw_string_end     AUTO

; Startup routine
hw_start:

; Set stack pointer. The comma is used as a number separator. 
; Underscore (_) and back-tick (`) may also be used as separators.
LD $0000,2000 S.H0

; Set base pointer
LD S.H0 S.H1

LD hw_string G
CALL stdlib_strupr

; Write string
CALL stdlib_strlen
LD A J
LD G H
LD $01 A
LD $01 G
INT $80                 ; Call interrupt $80 to execute write syscall

; "Power down" the system
LD $A9 A                ; Load syscall opcode $A9 (169, sys_reboot) into A register
LD $4321FEDC J          ; Load "magic" code for power down ($4321FEDC) into J register
INT $80

; This label points to the start of the string we want to output.
hw_string: 
STRING "Hello, world!\0""
