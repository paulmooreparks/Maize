LABEL stdlib_strlen AUTO
LABEL hw_string AUTO

; Execution begins at address $00000000:00000000

; Set stack pointer. The back-tick (`)  is used as a number separator. 
; Underscore (_) and comma (,) may also be used as separators.
ld $0000`1100 sp

; Set base pointer
ld sp bp

ld hw_string g
call stdlib_strlen
ld $01 g
ld hw_string h.h0
ld a.h0 j
sys $01
halt

hw_string: 
STRING "Hello, world!\0"  

LABEL stdlib_strlen_loop AUTO
LABEL stdlib_strlen_done AUTO

stdlib_strlen:
push g.h0
clr a
stdlib_strlen_loop:
clr b.b0
cmpind b.b0 @g.h0
jz stdlib_strlen_done
inc a.h0
inc g.h0
jmp stdlib_strlen_loop
stdlib_strlen_done:
pop g.h0
ret
