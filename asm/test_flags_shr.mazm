; **********************************************************************************
; SHR shift-count edge-case regression test for card maize-1 (byte width).
;
; The old code passed the raw count straight to C++'s >>, which is undefined behavior
; once the count reaches the operand width. This test exercises the counts the spec
; pins down: n=0, n=1, n=bits-1 (7), n=bits (8), n=bits+1 (9), and checks the flags.
;
; Value under test is $81 (1000_0001). Flags are read from RF.B0
; (C=$01, N=$02, V=$04, Z=$10) and isolated with AND. Any mismatch jumps to fail.
;
; Expected (SHR $81):
;   n=0  flags unaffected
;   n=1  result $40  : C set (bit0 out), V set (prior sign bit was set)
;   n=7  result $01  : C clear (bit6 out), V clear
;   n=8  result $00  : C set (bit7 out), Z set
;   n=9  result $00  : C clear, V clear, Z set
;
; Split from a combined SHL/SHR test to sidestep a layout-sensitive mazm back-patch
; defect (unrelated to this card; mazm is out of scope).
; **********************************************************************************

$0000`0000:
    ; ---- SHR n=0 : flags unaffected (C preserved set) ----
    CP $81 R0.B0
    SETCRY
    SHR $00 R0.B0
    JB shr0a_ok
    JMP fail
shr0a_ok:
    ; ---- SHR n=0 : C preserved clear ----
    CP $81 R0.B0
    CLRCRY
    SHR $00 R0.B0
    JB fail

    ; ---- SHR n=1 : C set, V set ----
    CP $81 R0.B0
    SHR $01 R0.B0
    CP RF.B0 R9.B0
    CP R9.B0 R8.B0
    AND $01 R8.B0
    JZ fail
    CP R9.B0 R8.B0
    AND $04 R8.B0
    JZ fail

    ; ---- SHR n=7 : C clear, V clear ----
    CP $81 R0.B0
    SHR $07 R0.B0
    CP RF.B0 R9.B0
    CP R9.B0 R8.B0
    AND $01 R8.B0
    JNZ fail
    CP R9.B0 R8.B0
    AND $04 R8.B0
    JNZ fail

    ; ---- SHR n=8 : C set, Z set ----
    CP $81 R0.B0
    SHR $08 R0.B0
    CP RF.B0 R9.B0
    CP R9.B0 R8.B0
    AND $10 R8.B0
    JZ fail
    CP R9.B0 R8.B0
    AND $01 R8.B0
    JZ fail

    ; ---- SHR n=9 : count > width, C clear, V clear, Z set ----
    CP $81 R0.B0
    SHR $09 R0.B0
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
    STRING "flags shr: PASS\n\0"

fail_string:
    STRING "flags shr: FAIL\n\0"
