LABEL stdlib_strlen     AUTO
LABEL hw_string         AUTO

; Execution begins at address $00000000:00000000

    LD hw_string G
    LD $4542 A.Q2
    ADD $01 A.Q2
    ST A.Q2 @G

    ; Set stack pointer. The back-tick (`)  is used as a number separator. 
    ; Underscore (_) and comma (,) may also be used as separators.
    LD $0000`1100 SP

    ; Set base pointer
    LD SP BP

    CALL stdlib_strlen
    LD $01 G
    LD hw_string H.H0
    LD A.H0 J
    SYS $01
    HALT

hw_string: 
   STRING "Hello, world!\0"  

LABEL stdlib_strlen_loop AUTO
LABEL stdlib_strlen_done AUTO

stdlib_strlen:
   PUSH G.H0
   CLR A
stdlib_strlen_loop:
   CLR B.B0
   CMPIND B.B0 @G.H0
   JZ stdlib_strlen_done
   INC A.H0
   INC G.H0
   JMP stdlib_strlen_loop
stdlib_strlen_done:
   POP G.H0
   RET

