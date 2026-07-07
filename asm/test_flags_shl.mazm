; **********************************************************************************
; SHL shift-count edge-case regression test for card maize-1 (byte width).
;
; The old code passed the raw count straight to C++'s <<, which is undefined behavior
; once the count reaches the operand width. This test exercises the counts the spec
; pins down: n=0, n=1, n=bits-1 (7), n=bits (8), n=bits+1 (9), and checks the flags.
;
; Value under test is $81 (1000_0001). Flags are read from RF.B0
; (C=$01, N=$02, V=$04, Z=$10) and isolated with AND. Any mismatch jumps to fail.
;
; Expected (SHL $81):
;   n=0  flags unaffected (C, preset via SETCRY/CLRCRY, is preserved)
;   n=1  result $02  : C set (bit7 out), V set (sign bit changed)
;   n=7  result $80  : C clear (bit1 out), V clear, N set
;   n=8  result $00  : C set (bit0 out), V clear, Z set
;   n=9  result $00  : C, V, N clear, Z set (count > width)
;
; Split from a combined SHL/SHR test to sidestep a layout-sensitive mazm back-patch
; defect (unrelated to this card; mazm is out of scope).
; **********************************************************************************

$0000`0000:
    ; ---- SHL n=0 : flags unaffected (C preserved set) ----
    CP $81 R0.B0
    SETCRY
    SHL $00 R0.B0
    JB shl0a_ok
    JMP fail
shl0a_ok:
    ; ---- SHL n=0 : C preserved clear ----
    CP $81 R0.B0
    CLRCRY
    SHL $00 R0.B0
    JB fail

    ; ---- SHL n=1 : C set, V set ----
    CP $81 R0.B0
    SHL $01 R0.B0
    CP RF.B0 R9.B0
    CP R9.B0 R8.B0
    AND $01 R8.B0
    JZ fail
    CP R9.B0 R8.B0
    AND $04 R8.B0
    JZ fail

    ; ---- SHL n=7 : C clear, V clear, N set ----
    CP $81 R0.B0
    SHL $07 R0.B0
    CP RF.B0 R9.B0
    CP R9.B0 R8.B0
    AND $01 R8.B0
    JNZ fail
    CP R9.B0 R8.B0
    AND $04 R8.B0
    JNZ fail
    CP R9.B0 R8.B0
    AND $02 R8.B0
    JZ fail

    ; ---- SHL n=8 : C set, V clear, Z set ----
    CP $81 R0.B0
    SHL $08 R0.B0
    CP RF.B0 R9.B0
    CP R9.B0 R8.B0
    AND $10 R8.B0
    JZ fail
    CP R9.B0 R8.B0
    AND $01 R8.B0
    JZ fail
    CP R9.B0 R8.B0
    AND $04 R8.B0
    JNZ fail

    ; ---- SHL n=9 : count > width, all cleared, Z set ----
    CP $81 R0.B0
    SHL $09 R0.B0
    CP RF.B0 R9.B0
    CP R9.B0 R8.B0
    AND $10 R8.B0
    JZ fail
    CP R9.B0 R8.B0
    AND $01 R8.B0
    JNZ fail
    CP R9.B0 R8.B0
    AND $04 R8.B0
    JNZ fail
    CP R9.B0 R8.B0
    AND $02 R8.B0
    JNZ fail

    ; ---- all checks passed ----
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP pass_string R1
    CP $10 R2               ; 16 bytes
    SYS $01
    HALT

fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_string R1
    CP $10 R2               ; 16 bytes
    SYS $01
    HALT

; **********************************************************************************
pass_string:
    STRING "flags shl: PASS\n\0"

fail_string:
    STRING "flags shl: FAIL\n\0"
