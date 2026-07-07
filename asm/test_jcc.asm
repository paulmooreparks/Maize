; **********************************************************************************
; Regression test for card maize-8: the branch complements JGE / JLE / JBE / JAE.
;
; CMP src dst computes (dst - src); the branch immediately follows so nothing disturbs the
; flags. Each complement is checked taken (at equality, the point of the <= / >= forms) and
; not-taken. JGE/JLE are signed (N vs V); JBE/JAE are unsigned (C, Z).
;
; NOTE: kept under 256 bytes on purpose. Programs that cross a 256-byte memory block fail
; today (a VM bug tracked separately), so this test cannot yet be larger.
; **********************************************************************************

$0000`0000:
    ; ---- JGE (>= signed): N == V ----
    CP $05 R0.B0
    CMP $05 R0.B0          ; 0 : N==V (equality)
    JGE jge_ok
    JMP fail
jge_ok:
    CP $03 R0.B0
    CMP $05 R0.B0          ; -2 : N!=V
    JGE fail               ; 3 >= 5 is false

    ; ---- JLE (<= signed): Z set or N != V ----
    CP $05 R0.B0
    CMP $05 R0.B0          ; Z set (equality)
    JLE jle_ok
    JMP fail
jle_ok:
    CP $05 R0.B0
    CMP $03 R0.B0          ; 2 : Z clear, N==V
    JLE fail               ; 5 <= 3 is false

    ; ---- JBE (<= unsigned): C set or Z set ----
    CP $05 R0.B0
    CMP $05 R0.B0          ; Z set (equality)
    JBE jbe_ok
    JMP fail
jbe_ok:
    CP $05 R0.B0
    CMP $03 R0.B0          ; no borrow, Z clear
    JBE fail               ; 5 <= 3 unsigned is false

    ; ---- JAE (>= unsigned): C clear ----
    CP $05 R0.B0
    CMP $05 R0.B0          ; C clear (equality)
    JAE jae_ok
    JMP fail
jae_ok:
    CP $03 R0.B0
    CMP $05 R0.B0          ; borrow, C set
    JAE fail               ; 3 >= 5 unsigned is false

    ; ---- all checks passed ----
    CP $01 R0
    CP pass_string R1.H0
    CP $0A R2
    SYS $01
    HALT

fail:
    CP $01 R0
    CP fail_string R1.H0
    CP $0A R2
    SYS $01
    HALT

pass_string:
    STRING "jcc: PASS\n\0"

fail_string:
    STRING "jcc: FAIL\n\0"
