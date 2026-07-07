; **********************************************************************************
; Regression test for card maize-5: signed DIV/MOD and unsigned UDIV/UMOD.
;
; DIV and MOD are signed (two's-complement, truncated toward zero; the remainder takes
; the sign of the dividend). UDIV and UMOD are unsigned. The byte $EC is -20 signed and
; 236 unsigned, so the two divisions of it by 3 produce different results, proving the
; signed/unsigned split.
; **********************************************************************************

$0000`0000:
    ; ---- signed DIV: -20 / 3 = -6 ($FA) ----
    CP $EC R0.B0           ; -20
    CP $03 R1.B0           ; 3
    DIV R1.B0 R0.B0        ; R0.B0 = -20 / 3 = -6
    CMP $FA R0.B0
    JNZ fail

    ; ---- signed MOD: -20 % 3 = -2 ($FE) ----
    CP $EC R2.B0           ; -20
    CP $03 R3.B0           ; 3
    MOD R3.B0 R2.B0        ; R2.B0 = -20 % 3 = -2
    CMP $FE R2.B0
    JNZ fail

    ; ---- signed DIV with a negative divisor: 20 / -3 = -6 ($FA) ----
    CP $14 R4.B0           ; 20
    CP $FD R5.B0           ; -3
    DIV R5.B0 R4.B0        ; R4.B0 = 20 / -3 = -6
    CMP $FA R4.B0
    JNZ fail

    ; ---- unsigned UDIV: 236 / 3 = 78 ($4E), differs from the signed result ----
    CP $EC R6.B0           ; 236
    CP $03 R7.B0           ; 3
    UDIV R7.B0 R6.B0       ; R6.B0 = 236 / 3 = 78
    CMP $4E R6.B0
    JNZ fail

    ; ---- unsigned UMOD: 236 % 3 = 2 ($02) ----
    CP $EC R8.B0           ; 236
    CP $03 R9.B0           ; 3
    UMOD R9.B0 R8.B0       ; R8.B0 = 236 % 3 = 2
    CMP $02 R8.B0
    JNZ fail

    ; ---- all checks passed ----
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP pass_string R1.H0
    CP $0A R2              ; 10 bytes
    SYS $01
    HALT

fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_string R1.H0
    CP $0A R2              ; 10 bytes
    SYS $01
    HALT

; **********************************************************************************
pass_string:
    STRING "div: PASS\n\0"

fail_string:
    STRING "div: FAIL\n\0"
