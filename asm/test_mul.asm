; **********************************************************************************
; MUL handler regression test for all width cases
; Verifies that MUL (multiply) works correctly at 1, 2, 4, and 8-byte widths
;
; Adjusted from the spec agent's draft (maize-2): a bare HALT gives no externally
; observable signal -- the pass path and every *_fail path all reached HALT with
; nothing to distinguish them. Added a direct stdout report (SYS $01, modeled on
; hello.asm's output call) at each exit point instead. This uses hardcoded string
; lengths rather than a strlen/print_string subroutine: a CALL-based strlen helper
; was tried first and hit what looks like a pre-existing, unrelated VM/assembler
; defect (a nested-CALL + far-forward-label combination silently corrupts a
; register), which is out of scope for maize-2's arithmetic-operator fix. Flagged
; separately in the card comments. The four multiplication tests and their
; expected values are otherwise unchanged from the original draft.
; **********************************************************************************

$0000`0000:
    CALL test_1byte
    CALL test_2byte
    CALL test_4byte
    CALL test_8byte
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP pass_string R1
    CP $1E R2           ; 30 bytes
    SYS $01
    HALT

; Test 1-byte multiplication
; Expected: 5 * 3 = 15 (0x0F)
test_1byte:
    PUSH BP
    CP SP BP

    CP $05 R0.B0        ; Load 5 into R0.B0
    CP $03 R1.B0        ; Load 3 into R1.B0
    MUL R1.B0 R0.B0     ; R0.B0 = R0.B0 * R1.B0 = 5 * 3 = 15

    CP $0F R1.B0        ; Load expected value (15) into R1.B0
    CMP R1.B0 R0.B0     ; Compare R0.B0 with 15
    JNZ test_1byte_fail

    CP BP SP
    POP BP
    RET

test_1byte_fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_1byte_string R1
    CP $1D R2           ; 29 bytes
    SYS $01
    HALT                ; Fail and stop

; Test 2-byte multiplication
; Expected: 100 * 50 = 5000 (0x1388)
test_2byte:
    PUSH BP
    CP SP BP

    CP $64 R0.Q0        ; Load 100 into R0.Q0 (quarter-word)
    CP $32 R1.Q0        ; Load 50 into R1.Q0
    MUL R1.Q0 R0.Q0     ; R0.Q0 = R0.Q0 * R1.Q0 = 100 * 50 = 5000

    CP $1388 R1.Q0      ; Load expected value (5000) into R1.Q0
    CMP R1.Q0 R0.Q0     ; Compare
    JNZ test_2byte_fail

    CP BP SP
    POP BP
    RET

test_2byte_fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_2byte_string R1
    CP $1D R2           ; 29 bytes
    SYS $01
    HALT

; Test 4-byte multiplication
; Expected: 1000 * 2000 = 2000000 (0x001E8480)
test_4byte:
    PUSH BP
    CP SP BP

    CP $3E8 R0.H0       ; Load 1000 into R0.H0 (half-word)
    CP $7D0 R1.H0       ; Load 2000 into R1.H0
    MUL R1.H0 R0.H0     ; R0.H0 = R0.H0 * R1.H0 = 1000 * 2000 = 2000000

    CP $1E8480 R1.H0    ; Load expected value (2000000) into R1.H0
    CMP R1.H0 R0.H0     ; Compare
    JNZ test_4byte_fail

    CP BP SP
    POP BP
    RET

test_4byte_fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_4byte_string R1
    CP $1D R2           ; 29 bytes
    SYS $01
    HALT

; Test 8-byte multiplication
; Expected: 100000 * 100000 = 10000000000 (0x00000002540BE400)
test_8byte:
    PUSH BP
    CP SP BP

    CP $186A0 R0         ; Load 100000 into R0 (full word)
    CP $186A0 R1         ; Load 100000 into R1
    MUL R1 R0            ; R0 = R0 * R1 = 100000 * 100000 = 10000000000

    CP $2540BE400 R1     ; Load expected value into R1
    CMP R1 R0            ; Compare
    JNZ test_8byte_fail

    CP BP SP
    POP BP
    RET

test_8byte_fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_8byte_string R1
    CP $1D R2           ; 29 bytes
    SYS $01
    HALT

; **********************************************************************************
; Messages (lengths above are computed excluding the trailing \0 terminator)

pass_string:
    STRING "MUL test: PASS (1/2/4/8-byte)\n\0"

fail_1byte_string:
    STRING "MUL test: FAIL (1-byte case)\n\0"

fail_2byte_string:
    STRING "MUL test: FAIL (2-byte case)\n\0"

fail_4byte_string:
    STRING "MUL test: FAIL (4-byte case)\n\0"

fail_8byte_string:
    STRING "MUL test: FAIL (8-byte case)\n\0"
