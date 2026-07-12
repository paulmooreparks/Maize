# Appendix B: Encoding Quick Reference

A one-page summary of the bit fields. Full detail is Chapters 5 and 6.

## B.1 The opcode byte

    %BBAA`AAAA
     ||  +----- bits 5..0: base opcode (0..63), 64 base slots
     ++-------- bits 7,6: mode bits

Mode bits, for an instruction whose source form varies:

    bit 6 = 0   source is a register
    bit 6 = 1   source is an immediate
    bit 7 = 0   source is a value
    bit 7 = 1   source is a memory address (@)

The four forms of a full-form instruction at base `$xx`:

    $0x  regVal    (register value)
    $4x  immVal    (immediate value)
    $8x  regAddr   (value at address in register, @Rn)
    $Cx  immAddr   (value at immediate address, @$nnnn)

In the row-packed condition families (Jcc, SETcc) and register-only unary families, the two
high bits instead select a row; see Appendix A and Chapter 6.

## B.2 The register operand byte

    %RRRR`SSSS
     |    +----- bits 3..0: subregister selector
     +---------- bits 7..4: register field

Register field (high nibble):

    $0..$9  R0..R9      $C  RF (flags)
    $A      RT          $D  RB / BP (base pointer)
    $B      RV          $E  RP / PC (program counter)
                        $F  RS / SP (stack pointer)

Subregister selector (low nibble):

    $0..$7  B0..B7   (1 byte each)
    $8..$B  Q0..Q3   (2 bytes each)
    $C      H0       (4 bytes, low half)
    $D      H1       (4 bytes, high half)
    $E      W0       (8 bytes, full register; a bare Rn means Rn.W0)
    $F      -> B0    (defined default; not an error)

Byte/field positions in the 64-bit register (low to high):

    offset  7   6   5   4   3   2   1   0
          [B7][B6][B5][B4][B3][B2][B1][B0]
          [ Q3   ][ Q2   ][ Q1   ][ Q0   ]
          [    H1        ][    H0        ]
          [           W0                 ]

## B.3 The immediate source operand byte

    %xxxx`x000  $x0  1-byte immediate  (8 bits)
    %xxxx`x001  $x1  2-byte immediate  (16 bits)
    %xxxx`x010  $x2  4-byte immediate  (32 bits)
    %xxxx`x011  $x3  8-byte immediate  (64 bits)

Bits 0..2 select the width; bit 3 and bits 4..7 are reserved (must be zero). An
immediate-size encoding of 4..7 decodes to the value-initialized default (not a trap).
Immediate bytes follow the operand bytes in little-endian order.

## B.4 The flag register FL (RF.H0)

    bit 0  C  carry / borrow
    bit 1  N  negative (sign of result)
    bit 2  V  signed overflow
    bit 3  P  parity / unordered (written only by FCMP; read by JP / SETP)
    bit 4  Z  zero
    bit 5  -  reserved
    bit 6  -  reserved

RF.H1 (privileged, written only in supervisor mode): privilege, interrupt-enabled,
interrupt-set, running.

## B.5 Worked example

`CP $FFCC4411 R3`:

    $41   $02   $3E   $11 $44 $CC $FF
    opcode: CP immVal reg (base $01, bit 6 set)
            $02: 4-byte immediate follows
                  $3E: destination R3 ($3) . W0 ($E)
                        immediate $FFCC4411, little-endian

CP sign-extends the 32-bit immediate to the 64-bit destination, so R3 = `$FFFFFFFFFFCC4411`.
