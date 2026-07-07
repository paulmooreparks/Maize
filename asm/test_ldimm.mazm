; **********************************************************************************
; Regression test for card maize-40: LD @imm reads the immediate address at its encoded
; width, not a fixed 8 bytes.
;
; The bug read 8 bytes for the address regardless of op1_imm_size, over-reading into the
; following instruction bytes and computing a garbage address for any address encoded in
; fewer than 8 bytes. This test stages a known byte at a low address (2-byte immediate)
; and at an address above 4 GiB (8-byte immediate, also exercising the maize-3 flat-64
; deref), then reads each back via LD @imm.
; **********************************************************************************

$0000`0000:
    ; ---- LD @imm with a 2-byte immediate address ----
    CP $2000 R0
    CP $5A R1.B0
    ST R1.B0 @R0           ; mem[0x0000_2000] = $5A
    LD @$2000 R2.B0        ; immediate-address load; the bug over-read the 2-byte address
    CMP $5A R2.B0
    JNZ fail

    ; ---- LD @imm with a 64-bit (>4 GiB) immediate address ----
    CP $5000 R3
    CP $01 R4
    SHL $20 R4
    OR R3 R4               ; R4 = 0x1_0000_5000
    CP $A5 R5.B0
    ST R5.B0 @R4           ; mem[0x1_0000_5000] = $A5
    LD @$1`0000`5000 R6.B0 ; 8-byte immediate address; reads the full 64-bit address
    CMP $A5 R6.B0
    JNZ fail

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
    STRING "ld imm: PASS\n\0"

fail_string:
    STRING "ld imm: FAIL\n\0"
