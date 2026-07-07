; **********************************************************************************
; Flag-neutrality regression test for card maize-4: data movement must not touch flags.
;
; Each case establishes a known flag state with CMP, then executes a data-movement
; instruction that under the old flags-on-load behavior would have overwritten Z, then
; branches on the flag CMP set. A move that clobbers the flag jumps to fail; reaching
; the end prints PASS.
;
;   CP (reg->reg) of a nonzero value must leave an equal CMP's Z set.
;   CLR of a register must leave an unequal CMP's Z clear.
;   LD (reg-indirect) of a nonzero byte must leave an equal CMP's Z set (the strlen case).
; **********************************************************************************

$0000`0000:
    ; ---- CP reg->reg preserves Z=1 from an equal CMP ----
    CP $05 R0.B0
    CP $05 R1.B0
    CMP R1.B0 R0.B0        ; 5 - 5 = 0 -> Z set
    CP $01 R3.B0           ; immediate load (stage a nonzero value)
    CP R3.B0 R2.B0         ; reg->reg move of nonzero; old behavior cleared Z here
    JZ cp_ok
    JMP fail
cp_ok:

    ; ---- CLR preserves Z=0 from an unequal CMP ----
    CP $05 R0.B0
    CP $03 R1.B0
    CMP R1.B0 R0.B0        ; 5 - 3 = 2 -> Z clear
    CLR R2                 ; old behavior set Z=1 here
    JNZ clr_ok
    JMP fail
clr_ok:

    ; ---- LD reg-indirect preserves Z=1 from an equal CMP (the strlen idiom) ----
    CP nonzero_byte R4.H0  ; stage the address (flag-neutral)
    CP $05 R0.B0
    CP $05 R1.B0
    CMP R1.B0 R0.B0        ; Z set
    LD @R4.H0 R2.B0        ; load $42; old behavior cleared Z here
    JZ ld_ok
    JMP fail
ld_ok:

    ; ---- all checks passed ----
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP pass_string R1
    CP $11 R2              ; 17 bytes
    SYS $01
    HALT

fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_string R1
    CP $11 R2              ; 17 bytes
    SYS $01
    HALT

; **********************************************************************************
nonzero_byte:
    DATA $42

pass_string:
    STRING "flags move: PASS\n\0"

fail_string:
    STRING "flags move: FAIL\n\0"
