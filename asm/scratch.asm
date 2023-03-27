LABEL stdlib_strlen     AUTO
LABEL hw_string         AUTO

; Execution begins at address $00000000:00000000

$00000000:
    ; Set stack pointer. The back-tick (`)  is used as a number separator. 
    ; Underscore (_) and comma (,) may also be used as separators.
    LD $0000`1100 SP            ;00001000

    ; Set base pointer
    LD SP BP                    ;00001007

    CLR G                       ;0000100A
    LD hw_string G              ;0000100C
    CALL stdlib_strlen          ;00001013
    LD $01 G                    ;00001019
    LD hw_string H.H0           ;0000101D
    LD A.H0 J                   ;00001024
    SYS $01                     ;00001027
    HALT                        ;0000102A

hw_string: 
   STRING "Hello, world!\0"     ;00001050

LABEL stdlib_strlen_loop AUTO
LABEL stdlib_strlen_done AUTO

stdlib_strlen:
   PUSH G.H0                    ;0000105E
   CLR A                        ;00001060
stdlib_strlen_loop:
   CLR B.B0
   CMPIND B.B0 @G.H0             ;00001062
   JZ stdlib_strlen_done        ;00001066
   INC A.H0                     ;0000106C
   INC G.H0                     ;0000106E
   JMP stdlib_strlen_loop       ;00001070
stdlib_strlen_done:
   POP G.H0                     ;00001076
   RET                          ;00001078

