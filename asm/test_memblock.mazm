; **********************************************************************************
; Regression test for card maize-42: multi-byte memory access across a 256-byte
; block boundary.
;
; memory_module caches one 256-byte block; the multi-byte write helpers used to
; advance cache_address.b0 without re-resolving the block, so any store that straddled
; a block boundary wrapped 0xFF -> 0x00 back into the SAME block and corrupted the
; upper bytes. The store address here is an absolute runtime address (in a register),
; independent of where this code is assembled, so the byte range genuinely crosses a
; 256-byte boundary. Each value is stored, read back, and required to match exactly.
;
;   - 8-byte store/load straddling 0x100 (bytes 0x00FC .. 0x0103).
;   - 4-byte store/load straddling 0x200 (bytes 0x01FE .. 0x0201).
;
; These stores exercise the same write_word / write_hword helpers mazm uses to emit
; multi-byte operands, so a green result also covers the assembler-side write path.
; **********************************************************************************

$0000`0000:
    ; ---- 8-byte store/load straddling the 0x100 block boundary ----
    CP $DEADBEEF R3.H0
    CP $CAFEF00D R3.H1        ; R3 = 0xCAFEF00D`DEADBEEF
    CP $00FC R0               ; 0x00FC .. 0x0103 straddles 0x100
    ST R3 @R0
    LD @R0 R4
    CMP R3 R4
    JNZ fail

    ; ---- 4-byte store/load straddling the 0x200 block boundary ----
    CP $12345678 R5.H0
    CP $01FE R1               ; 0x01FE .. 0x0201 straddles 0x200
    ST R5.H0 @R1
    LD @R1 R6.H0
    CMP $12345678 R6.H0
    JNZ fail

    ; ---- all checks passed ----
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP pass_string R1.H0
    CP $0F R2                 ; 15 bytes ("memblock: PASS\n")
    SYS $01
    HALT

fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_string R1.H0
    CP $0F R2                 ; 15 bytes ("memblock: FAIL\n")
    SYS $01
    HALT

; **********************************************************************************
pass_string:
    STRING "memblock: PASS\n\0"

fail_string:
    STRING "memblock: FAIL\n\0"
