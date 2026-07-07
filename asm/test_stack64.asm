; **********************************************************************************
; Regression test for card maize-41: 64-bit stack and control flow.
;
; PUSH/POP must round-trip a full 64-bit value (the stack pointer is now 64-bit), and
; CALL/RET must round-trip a full 64-bit return address so execution resumes at the
; instruction after the CALL.
; **********************************************************************************

$0000`0000:
    ; ---- PUSH/POP round-trips a full 64-bit value ----
    CP $DEADBEEF R0.H0
    CP $CAFEF00D R0.H1     ; R0 = 0xCAFEF00D`DEADBEEF
    PUSH R0
    CLR R1
    POP R1                 ; R1 must equal R0 across all 8 bytes
    CMP R0 R1
    JNZ fail

    ; ---- CALL/RET returns to the instruction after the CALL ----
    CLR R4
    CALL sub_set           ; sub_set sets R4.B0 = $01 and returns here
    CMP $01 R4.B0
    JNZ fail

    ; ---- all checks passed ----
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP pass_string R1.H0
    CP $0E R2              ; 14 bytes
    SYS $01
    HALT

sub_set:
    CP $01 R4.B0
    RET

fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_string R1.H0
    CP $0E R2              ; 14 bytes
    SYS $01
    HALT

; **********************************************************************************
pass_string:
    STRING "stack64: PASS\n\0"

fail_string:
    STRING "stack64: FAIL\n\0"
