; **********************************************************************************
; Conditional-branch dispatch regression test for card maize-1.
;
; Exercises the newly-wired JB / JA / JLT / JGT dispatch cases plus SETCRY / CLRCRY,
; checking both the taken and not-taken directions after a byte-width CMP:
;
;   JB  taken     dst=$01 <u src=$FF  (borrow, C set)
;   JB  not-taken dst=$FF >u src=$01  (no borrow, C clear)
;   JA  taken     dst=$FF >u src=$01  (C clear, Z clear)
;   JA  not-taken dst=$01 <u src=$FF  (C set)
;   JLT taken     dst=$80 <s src=$01  (-128 < 1; N != V)
;   JLT not-taken dst=$01,  src=$80   (1 vs -128; N == V)
;   JGT taken     dst=$05 >s src=$03  (5 > 3; Z clear, N == V)
;   JGT not-taken dst=$03,  src=$05   (3 vs 5; N != V) -- the regression case the
;                                       old copy-pasted JLT formula would misfire on
;   SETCRY then JB taken; CLRCRY then JB not-taken
;
; CMP src dst computes (dst - src); the branch immediately follows so no intervening
; instruction disturbs the flags. Any wrong outcome jumps to fail; the end prints PASS.
; **********************************************************************************

$0000`0000:
    ; ---- JB taken (C set) ----
    CP $01 R0.B0
    CP $FF R1.B0
    CMP R1.B0 R0.B0        ; $01 - $FF : borrow, C set
    JB jb_taken_ok
    JMP fail
jb_taken_ok:

    ; ---- JB not-taken (C clear) ----
    CP $FF R0.B0
    CP $01 R1.B0
    CMP R1.B0 R0.B0        ; $FF - $01 : no borrow, C clear
    JB fail                ; must NOT jump

    ; ---- JA taken (C clear, Z clear) ----
    CP $FF R0.B0
    CP $01 R1.B0
    CMP R1.B0 R0.B0        ; C clear, Z clear
    JA ja_taken_ok
    JMP fail
ja_taken_ok:

    ; ---- JA not-taken (C set) ----
    CP $01 R0.B0
    CP $FF R1.B0
    CMP R1.B0 R0.B0        ; C set
    JA fail                ; must NOT jump

    ; ---- JLT taken (N != V) ----
    CP $80 R0.B0
    CP $01 R1.B0
    CMP R1.B0 R0.B0        ; -128 - 1 : N=0, V=1 -> N != V
    JLT jlt_taken_ok
    JMP fail
jlt_taken_ok:

    ; ---- JLT not-taken (N == V) ----
    CP $01 R0.B0
    CP $80 R1.B0
    CMP R1.B0 R0.B0        ; 1 - (-128) : N=1, V=1 -> N == V
    JLT fail               ; must NOT jump

    ; ---- JGT taken (Z clear, N == V) ----
    CP $05 R0.B0
    CP $03 R1.B0
    CMP R1.B0 R0.B0        ; 5 - 3 = 2 : N=0, V=0, Z=0
    JGT jgt_taken_ok
    JMP fail
jgt_taken_ok:

    ; ---- JGT not-taken : dst=3, src=5 regression case ----
    CP $03 R0.B0
    CP $05 R1.B0
    CMP R1.B0 R0.B0        ; 3 - 5 : N=1, V=0 -> N != V (old formula would fire)
    JGT fail               ; must NOT jump

    ; ---- SETCRY then JB taken ----
    SETCRY
    JB setcry_ok
    JMP fail
setcry_ok:

    ; ---- CLRCRY then JB not-taken ----
    CLRCRY
    JB fail                ; must NOT jump

    ; ---- all checks passed ----
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP pass_string R1
    CP $13 R2              ; 19 bytes
    SYS $01
    HALT

fail:
    CLR R0
    CLR R1
    CLR R2
    CP $01 R0
    CP fail_string R1
    CP $13 R2              ; 19 bytes
    SYS $01
    HALT

; **********************************************************************************
pass_string:
    STRING "flags branch: PASS\n\0"

fail_string:
    STRING "flags branch: FAIL\n\0"
