; **********************************************************************************
; Regression test for card maize-40: CMP and TEST must not modify their destination.
;
; Both set flags only. The bug wrote the ALU result (the subtraction for CMP, the AND
; for TEST) back into the destination register. Each case sets a known destination,
; runs the compare, then re-checks the destination still holds its original value.
; **********************************************************************************

$0000`0000:
    ; ---- CMP leaves its destination unchanged ----
    CP $37 R0.B0           ; destination holds $37
    CP $05 R1.B0           ; source
    CMP R1.B0 R0.B0        ; flags from $37 - $05; must NOT alter R0.B0
    CMP $37 R0.B0          ; R0.B0 must still equal $37 -> Z set
    JNZ fail               ; the bug left R0.B0 = $32 (the subtraction result)

    ; ---- TEST leaves its destination unchanged ----
    CP $33 R2.B0           ; destination holds $33
    CP $0F R3.B0           ; source
    TEST R3.B0 R2.B0       ; flags from $33 & $0F; must NOT alter R2.B0
    CMP $33 R2.B0          ; R2.B0 must still equal $33 -> Z set
    JNZ fail               ; the bug left R2.B0 = $03 (the AND result)

    ; ---- all checks passed ----
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP pass_string R1
    CP $0E R2              ; 14 bytes
    SYS $01
    HALT

fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_string R1
    CP $0E R2              ; 14 bytes
    SYS $01
    HALT

; **********************************************************************************
pass_string:
    STRING "cmptest: PASS\n\0"

fail_string:
    STRING "cmptest: FAIL\n\0"
