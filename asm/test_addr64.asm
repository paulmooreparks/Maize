; **********************************************************************************
; Flat 64-bit addressing regression test for card maize-3.
;
; Maize uses a flat 64-bit address space; segments are a privilege boundary only and
; are not part of address arithmetic. This test proves data loads and stores honor the
; full 64-bit address rather than truncating to the low 32 bits.
;
; It writes distinct bytes to two addresses that share the same low 32 bits but differ
; in bit 32 (0x0000_2000 and 0x1_0000_2000). Under 32-bit address truncation the two
; would alias and the second store would clobber the first; under flat-64 they are
; distinct. Any mismatch jumps to fail; reaching the end prints PASS.
; **********************************************************************************

$0000`0000:
    ; ---- build the two addresses ----
    CP $2000 R0            ; low data address 0x0000_2000
    CP $01 R1
    SHL $20 R1             ; R1 = 1 << 32 = 0x1_0000_0000
    OR R0 R1               ; R1 = 0x1_0000_2000 (same low 32 bits as R0, bit 32 set)

    ; ---- store distinct bytes ----
    CP $11 R3.B0
    ST R3.B0 @R0           ; mem[0x0000_2000] = $11
    CP $22 R3.B0
    ST R3.B0 @R1           ; mem[0x1_0000_2000] = $22  (must not alias the low address)

    ; ---- read back and verify both survived ----
    LD @R0 R4.B0           ; reload low
    CMP $11 R4.B0
    JNZ fail               ; low byte must still be $11 (not clobbered by the high store)
    LD @R1 R5.B0           ; reload high
    CMP $22 R5.B0
    JNZ fail               ; high byte must be $22

    ; ---- all checks passed ----
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP pass_string R1
    CP $0D R2              ; 13 bytes
    SYS $01
    HALT

fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_string R1
    CP $0D R2              ; 13 bytes
    SYS $01
    HALT

; **********************************************************************************
pass_string:
    STRING "addr64: PASS\n\0"

fail_string:
    STRING "addr64: FAIL\n\0"
