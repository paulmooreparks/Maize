; **********************************************************************************
; Width-8 (W) MUL overflow regression test for card maize-1.
;
; The old code computed MUL's overflow (V) at W width from the .h0 (32-bit)
; subregisters instead of .w0 (64-bit), so V reflected only the low 32 bits of each
; operand. This test uses operands whose 64-bit and low-32-bit interpretations give
; a DIFFERENT overflow result, confirming V now reflects the full 64-bit computation.
;
;   Case A: 2^32 * 2^32  -> true 64-bit product is 2^64 (overflow). Nonzero high 32
;           bits; V must be SET. (The old .h0 read saw 0 * 0, no overflow.)
;   Case B: $FFFFFFFF * 2 -> product $1FFFFFFFE fits in 64 bits (no overflow); V must
;           be CLEAR. (The old .h0 read of $FFFFFFFF * 2 as 32-bit reported overflow.)
;
; Registers are CLR-ed before each case: CP of a short immediate into a full register
; only overwrites the low byte(s), so stale high bytes from a prior value would
; otherwise corrupt the 64-bit operand.
;
; Flags read from RF.B0 (V=$04). Any mismatch jumps to fail; the end prints PASS.
; **********************************************************************************

$0000`0000:
    ; ---- Case A: 2^32 * 2^32 overflows at 64 bits -> V set ----
    CLR R0
    CLR R1
    CP $100000000 R0        ; 2^32
    CP $100000000 R1        ; 2^32
    MUL R1 R0               ; W-width multiply, 64-bit operands
    CP RF.B0 R9.B0
    CP R9.B0 R8.B0
    AND $04 R8.B0           ; isolate V
    JZ fail                 ; V must be set

    ; ---- Case B: $FFFFFFFF * 2 fits in 64 bits -> V clear ----
    ; Build $00000000FFFFFFFF without sign-extending a 4-byte immediate: load 2^32
    ; (an 8-byte immediate, no sign extension) and decrement to $FFFFFFFF.
    CLR R0
    CLR R1
    CP $100000000 R0
    DEC R0                  ; R0 = $00000000FFFFFFFF
    CP $02 R1
    MUL R1 R0               ; product $1FFFFFFFE, no 64-bit overflow
    CP RF.B0 R9.B0
    CP R9.B0 R8.B0
    AND $04 R8.B0           ; isolate V
    JNZ fail                ; V must be clear (a 32-bit .h0 test would have set it)

    ; ---- all checks passed ----
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP pass_string R1
    CP $11 R2               ; 17 bytes
    SYS $01
    HALT

fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_string R1
    CP $11 R2               ; 17 bytes
    SYS $01
    HALT

; **********************************************************************************
pass_string:
    STRING "flags mul8: PASS\n\0"

fail_string:
    STRING "flags mul8: FAIL\n\0"
