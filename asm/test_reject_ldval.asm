; **********************************************************************************
; Negative test for card maize-43: the assembler must REJECT a data-movement mnemonic
; used against the wrong operand kind. Here LD is given a value source (no '@'), which
; is a copy, not a load. mazm must fail with a diagnostic telling the author to use CP.
; The test harness treats this file as ExpectAsmError: it passes when assembly fails.
; **********************************************************************************

$0000`0000:
    LD $5 R0                ; error: LD reads from a memory address; use CP for a value
    HALT
