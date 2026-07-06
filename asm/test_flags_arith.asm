; **********************************************************************************
; Flag-boundary regression test for card maize-1: ADD/SUB carry (C) and overflow (V).
;
; Verifies that, at byte (B) width:
;   $FF + $01  sets C (unsigned carry-out) and clears V (no signed overflow)
;   $7F + $01  sets V (signed overflow; result crosses the signed boundary)
;   $80 - $01  sets V (signed overflow on subtraction)
;   $00 - $01  sets C (unsigned borrow)
;
; The flags are read back from RF.B0 (C=bit0=$01, N=bit1=$02, V=bit2=$04, Z=bit4=$10)
; and each required bit is isolated with AND before branching. Any mismatch jumps to
; the single fail path; reaching the end prints PASS.
;
; RF.B0 bit layout: C=$01  N=$02  V=$04  P=$08  Z=$10
; **********************************************************************************

$0000`0000:
    ; ---- $FF + $01 : C set, V clear ----
    CP $FF R0.B0
    CP $01 R1.B0
    ADD R1.B0 R0.B0         ; R0.B0 = $FF + $01 = $00
    CP RF.B0 R9.B0          ; snapshot flags before any flag-touching instruction
    CP R9.B0 R8.B0
    AND $01 R8.B0           ; isolate C
    JZ fail                 ; C must be set
    CP R9.B0 R8.B0
    AND $04 R8.B0           ; isolate V
    JNZ fail                ; V must be clear

    ; ---- $7F + $01 : V set ----
    CP $7F R0.B0
    CP $01 R1.B0
    ADD R1.B0 R0.B0         ; R0.B0 = $7F + $01 = $80
    CP RF.B0 R9.B0
    CP R9.B0 R8.B0
    AND $04 R8.B0           ; isolate V
    JZ fail                 ; V must be set

    ; ---- $80 - $01 : V set ----
    CP $80 R0.B0
    CP $01 R1.B0
    SUB R1.B0 R0.B0         ; R0.B0 = $80 - $01 = $7F
    CP RF.B0 R9.B0
    CP R9.B0 R8.B0
    AND $04 R8.B0           ; isolate V
    JZ fail                 ; V must be set

    ; ---- $00 - $01 : C set (borrow) ----
    CP $00 R0.B0
    CP $01 R1.B0
    SUB R1.B0 R0.B0         ; R0.B0 = $00 - $01 = $FF
    CP RF.B0 R9.B0
    CP R9.B0 R8.B0
    AND $01 R8.B0           ; isolate C
    JZ fail                 ; C must be set

    ; ---- all checks passed ----
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP pass_string R1
    CP $12 R2               ; 18 bytes
    SYS $01
    HALT

fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_string R1
    CP $12 R2               ; 18 bytes
    SYS $01
    HALT

; **********************************************************************************
pass_string:
    STRING "flags arith: PASS\n\0"

fail_string:
    STRING "flags arith: FAIL\n\0"
