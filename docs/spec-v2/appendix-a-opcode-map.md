# Appendix A: Opcode Map

This appendix is normative. It assigns every one of the 256 bytes of the primary opcode
page, and it is the companion to the function-grouped instruction inventory: the inventory
says what each instruction does, and this appendix says which byte spells it and how long
the instruction is.

The map is organized as contiguous bands, one per family, each with reserved headroom at its
top. A reader can therefore tell a family from the high nibble of an opcode in most cases,
and an implementer can dispatch on ranges as readily as on a table. The base freezes once, so
the reserved entries below are not a to-do list for later base revisions; they exist so that
future extension pages can be opened without disturbing anything already assigned, and so
that a mistake found before the freeze has somewhere to land.

The **Form** column names the length class from the instruction-encoding chapter, where `op`
is the opcode byte, `r` is one operand byte, and `iN` is an immediate of N bytes. The
**Len** column gives the total instruction length in bytes, and it is the value a decoder
reads from the 256-entry length table.

The Operands column is assembly order and the Form column is byte order, and the two
deliberately diverge wherever an immediate is a source: assembly writes operands source to
destination, so a source immediate appears before the destination register, while the
encoding always places every immediate after every operand byte. The form `op r i8` under
`move.w $imm rd` is therefore correct: the operand byte naming rd precedes the eight immediate
bytes in memory, per the fixed component order the instruction-encoding chapter states.

Every byte marked reserved raises the illegal-instruction trap when fetched as an
instruction, reporting the offending byte and the address of that byte. No reserved byte
executes as a no-operation, and no reserved byte has a defaulted interpretation.

## A.1 Band summary

| Range | Family | Assigned | Reserved |
|:------|:-------|:--------:|:--------:|
| `$00` | Zero-byte guard | 0 | 1 |
| `$01`..`$0F` | Constants and moves | 9 | 6 |
| `$10`..`$3F` | Integer arithmetic and logic | 46 | 2 |
| `$40`..`$5F` | Compares | 20 | 12 |
| `$60`..`$6F` | Branches | 10 | 6 |
| `$70`..`$7F` | Control transfer and select | 7 | 9 |
| `$80`..`$9F` | Loads and stores | 22 | 10 |
| `$A0`..`$AF` | Extract and insert | 12 | 4 |
| `$B0`..`$B7` | Block memory | 3 | 5 |
| `$B8`..`$C7` | System, control registers, TLB, ports | 12 | 4 |
| `$C8`..`$F7` | Floating point | 44 | 4 |
| `$F8`..`$FE` | Extension escape bytes | 7 escapes | 0 |
| `$FF` | Breakpoint | 1 | 0 |

The totals are 186 assigned instruction opcodes, 7 escape bytes, and 63 reserved bytes.

## A.2 The zero-byte guard

`$00` is reserved and traps. A run of zeroed memory reached as code therefore raises the
illegal-instruction trap at its first byte rather than executing anything, which is a
deliberate change from v1, where `$00` was `HALT`. Trapping contains a runaway program at
least as well as halting does and it tells the operator why the machine stopped, so v2
spends the byte on containment rather than on an instruction.

## A.3 Constants and moves, `$01`..`$0F`

| Byte | Mnemonic | Operands | Form | Len |
|:----:|:---------|:---------|:-----|:---:|
| `$01` | `move` | `move rs rd` | `op r r` | 3 |
| `$02` | `move.w` | `move.w $imm rd` | `op r i8` | 10 |
| `$03` | `move.zb` | `move.zb $imm rd` | `op r i1` | 3 |
| `$04` | `move.sb` | `move.sb $imm rd` | `op r i1` | 3 |
| `$05` | `move.zq` | `move.zq $imm rd` | `op r i2` | 4 |
| `$06` | `move.sq` | `move.sq $imm rd` | `op r i2` | 4 |
| `$07` | `move.zh` | `move.zh $imm rd` | `op r i4` | 6 |
| `$08` | `move.sh` | `move.sh $imm rd` | `op r i4` | 6 |
| `$09` | `pc_add` | `pc_add $imm rd` | `op r i4` | 6 |
| `$0A`..`$0F` | reserved | | | |

## A.4 Integer arithmetic and logic, `$10`..`$3F`

The band runs register-to-register forms first, then the unary forms, then the carry pair,
then the immediate forms, so a translator can range-test the group it cares about.

| Byte | Mnemonic | Operands | Form | Len |
|:----:|:---------|:---------|:-----|:---:|
| `$10` | `add` | `add rs1 rs2 rd` | `op r r r` | 4 |
| `$11` | `add.h` | `add.h rs1 rs2 rd` | `op r r r` | 4 |
| `$12` | `subtract` | `subtract rs1 rs2 rd` | `op r r r` | 4 |
| `$13` | `subtract.h` | `subtract.h rs1 rs2 rd` | `op r r r` | 4 |
| `$14` | `multiply` | `multiply rs1 rs2 rd` | `op r r r` | 4 |
| `$15` | `multiply.h` | `multiply.h rs1 rs2 rd` | `op r r r` | 4 |
| `$16` | `multiply_high_signed` | `multiply_high_signed rs1 rs2 rd` | `op r r r` | 4 |
| `$17` | `multiply_high_unsigned` | `multiply_high_unsigned rs1 rs2 rd` | `op r r r` | 4 |
| `$18` | `divide_signed` | `divide_signed rs1 rs2 rd` | `op r r r` | 4 |
| `$19` | `divide_signed.h` | `divide_signed.h rs1 rs2 rd` | `op r r r` | 4 |
| `$1A` | `divide_unsigned` | `divide_unsigned rs1 rs2 rd` | `op r r r` | 4 |
| `$1B` | `divide_unsigned.h` | `divide_unsigned.h rs1 rs2 rd` | `op r r r` | 4 |
| `$1C` | `remainder_signed` | `remainder_signed rs1 rs2 rd` | `op r r r` | 4 |
| `$1D` | `remainder_signed.h` | `remainder_signed.h rs1 rs2 rd` | `op r r r` | 4 |
| `$1E` | `remainder_unsigned` | `remainder_unsigned rs1 rs2 rd` | `op r r r` | 4 |
| `$1F` | `remainder_unsigned.h` | `remainder_unsigned.h rs1 rs2 rd` | `op r r r` | 4 |
| `$20` | `and` | `and rs1 rs2 rd` | `op r r r` | 4 |
| `$21` | `or` | `or rs1 rs2 rd` | `op r r r` | 4 |
| `$22` | `xor` | `xor rs1 rs2 rd` | `op r r r` | 4 |
| `$23` | `shift_left` | `shift_left rs1 rs2 rd` | `op r r r` | 4 |
| `$24` | `shift_left.h` | `shift_left.h rs1 rs2 rd` | `op r r r` | 4 |
| `$25` | `shift_right_logical` | `shift_right_logical rs1 rs2 rd` | `op r r r` | 4 |
| `$26` | `shift_right_logical.h` | `shift_right_logical.h rs1 rs2 rd` | `op r r r` | 4 |
| `$27` | `shift_right_arithmetic` | `shift_right_arithmetic rs1 rs2 rd` | `op r r r` | 4 |
| `$28` | `shift_right_arithmetic.h` | `shift_right_arithmetic.h rs1 rs2 rd` | `op r r r` | 4 |
| `$29` | `not` | `not rs rd` | `op r r` | 3 |
| `$2A` | `not.h` | `not.h rs rd` | `op r r` | 3 |
| `$2B` | `negate` | `negate rs rd` | `op r r` | 3 |
| `$2C` | `negate.h` | `negate.h rs rd` | `op r r` | 3 |
| `$2D` | `byte_reverse` | `byte_reverse rs rd` | `op r r` | 3 |
| `$2E` | `byte_reverse.h` | `byte_reverse.h rs rd` | `op r r` | 3 |
| `$2F` | `add_carry` | `add_carry rs1 rs2 rc rd` | `op r r r r` | 5 |
| `$30` | `subtract_borrow` | `subtract_borrow rs1 rs2 rc rd` | `op r r r r` | 5 |
| `$31` | `add` | `add rs $imm rd` | `op r r i4` | 7 |
| `$32` | `add.h` | `add.h rs $imm rd` | `op r r i4` | 7 |
| `$33` | `subtract` | `subtract rs $imm rd` | `op r r i4` | 7 |
| `$34` | `subtract.h` | `subtract.h rs $imm rd` | `op r r i4` | 7 |
| `$35` | `and` | `and rs $imm rd` | `op r r i4` | 7 |
| `$36` | `or` | `or rs $imm rd` | `op r r i4` | 7 |
| `$37` | `xor` | `xor rs $imm rd` | `op r r i4` | 7 |
| `$38` | `shift_left` | `shift_left rs #imm rd` | `op r r i1` | 4 |
| `$39` | `shift_left.h` | `shift_left.h rs #imm rd` | `op r r i1` | 4 |
| `$3A` | `shift_right_logical` | `shift_right_logical rs #imm rd` | `op r r i1` | 4 |
| `$3B` | `shift_right_logical.h` | `shift_right_logical.h rs #imm rd` | `op r r i1` | 4 |
| `$3C` | `shift_right_arithmetic` | `shift_right_arithmetic rs #imm rd` | `op r r i1` | 4 |
| `$3D` | `shift_right_arithmetic.h` | `shift_right_arithmetic.h rs #imm rd` | `op r r i1` | 4 |
| `$3E`..`$3F` | reserved | | | |

## A.5 Compares, `$40`..`$5F`

The ten register-to-register predicates occupy `$40`..`$49` and the same ten predicates in
the immediate form occupy `$4A`..`$53`, in the identical order, so the immediate opcode is
the register opcode plus ten.

| Byte | Mnemonic | Operands | Form | Len |
|:----:|:---------|:---------|:-----|:---:|
| `$40` | `compare_eq` | `compare_eq rs1 rs2 rd` | `op r r r` | 4 |
| `$41` | `compare_ne` | `compare_ne rs1 rs2 rd` | `op r r r` | 4 |
| `$42` | `compare_lt_signed` | `compare_lt_signed rs1 rs2 rd` | `op r r r` | 4 |
| `$43` | `compare_le_signed` | `compare_le_signed rs1 rs2 rd` | `op r r r` | 4 |
| `$44` | `compare_gt_signed` | `compare_gt_signed rs1 rs2 rd` | `op r r r` | 4 |
| `$45` | `compare_ge_signed` | `compare_ge_signed rs1 rs2 rd` | `op r r r` | 4 |
| `$46` | `compare_lt_unsigned` | `compare_lt_unsigned rs1 rs2 rd` | `op r r r` | 4 |
| `$47` | `compare_le_unsigned` | `compare_le_unsigned rs1 rs2 rd` | `op r r r` | 4 |
| `$48` | `compare_gt_unsigned` | `compare_gt_unsigned rs1 rs2 rd` | `op r r r` | 4 |
| `$49` | `compare_ge_unsigned` | `compare_ge_unsigned rs1 rs2 rd` | `op r r r` | 4 |
| `$4A` | `compare_eq` | `compare_eq rs $imm rd` | `op r r i4` | 7 |
| `$4B` | `compare_ne` | `compare_ne rs $imm rd` | `op r r i4` | 7 |
| `$4C` | `compare_lt_signed` | `compare_lt_signed rs $imm rd` | `op r r i4` | 7 |
| `$4D` | `compare_le_signed` | `compare_le_signed rs $imm rd` | `op r r i4` | 7 |
| `$4E` | `compare_gt_signed` | `compare_gt_signed rs $imm rd` | `op r r i4` | 7 |
| `$4F` | `compare_ge_signed` | `compare_ge_signed rs $imm rd` | `op r r i4` | 7 |
| `$50` | `compare_lt_unsigned` | `compare_lt_unsigned rs $imm rd` | `op r r i4` | 7 |
| `$51` | `compare_le_unsigned` | `compare_le_unsigned rs $imm rd` | `op r r i4` | 7 |
| `$52` | `compare_gt_unsigned` | `compare_gt_unsigned rs $imm rd` | `op r r i4` | 7 |
| `$53` | `compare_ge_unsigned` | `compare_ge_unsigned rs $imm rd` | `op r r i4` | 7 |
| `$54`..`$5F` | reserved | | | |

## A.6 Branches, `$60`..`$6F`

The branch predicates occupy `$60`..`$69` in the same order as the compare predicates, so
the branch opcode is the register-form compare opcode plus `$20`. That relationship is a
convenience for a reader and a translator, and it is not load-bearing: the length table and
the dispatch read the byte itself.

| Byte | Mnemonic | Operands | Form | Len |
|:----:|:---------|:---------|:-----|:---:|
| `$60` | `branch_eq` | `branch_eq rs1 rs2 target` | `op r r i4` | 7 |
| `$61` | `branch_ne` | `branch_ne rs1 rs2 target` | `op r r i4` | 7 |
| `$62` | `branch_lt_signed` | `branch_lt_signed rs1 rs2 target` | `op r r i4` | 7 |
| `$63` | `branch_le_signed` | `branch_le_signed rs1 rs2 target` | `op r r i4` | 7 |
| `$64` | `branch_gt_signed` | `branch_gt_signed rs1 rs2 target` | `op r r i4` | 7 |
| `$65` | `branch_ge_signed` | `branch_ge_signed rs1 rs2 target` | `op r r i4` | 7 |
| `$66` | `branch_lt_unsigned` | `branch_lt_unsigned rs1 rs2 target` | `op r r i4` | 7 |
| `$67` | `branch_le_unsigned` | `branch_le_unsigned rs1 rs2 target` | `op r r i4` | 7 |
| `$68` | `branch_gt_unsigned` | `branch_gt_unsigned rs1 rs2 target` | `op r r i4` | 7 |
| `$69` | `branch_ge_unsigned` | `branch_ge_unsigned rs1 rs2 target` | `op r r i4` | 7 |
| `$6A`..`$6F` | reserved | | | |

## A.7 Control transfer and select, `$70`..`$7F`

| Byte | Mnemonic | Operands | Form | Len |
|:----:|:---------|:---------|:-----|:---:|
| `$70` | `jump` | `jump target` | `op i4` | 5 |
| `$71` | `jump` | `jump rs` | `op r` | 2 |
| `$72` | `call` | `call target` | `op i4` | 5 |
| `$73` | `call` | `call rs` | `op r` | 2 |
| `$74` | `return` | `return` | `op` | 1 |
| `$75` | `select_nz` | `select_nz rs rc rd` | `op r r r` | 4 |
| `$76` | `select_z` | `select_z rs rc rd` | `op r r r` | 4 |
| `$77`..`$7F` | reserved | | | |

## A.8 Loads and stores, `$80`..`$9F`

The band runs the seven bare loads, then the same seven loads with a displacement, then the
four bare stores, then the four displaced stores, so the displaced opcode is the bare opcode
plus seven for a load and plus four for a store.

| Byte | Mnemonic | Operands | Form | Len |
|:----:|:---------|:---------|:-----|:---:|
| `$80` | `load` | `load @rb rd` | `op r r` | 3 |
| `$81` | `load.zb` | `load.zb @rb rd` | `op r r` | 3 |
| `$82` | `load.sb` | `load.sb @rb rd` | `op r r` | 3 |
| `$83` | `load.zq` | `load.zq @rb rd` | `op r r` | 3 |
| `$84` | `load.sq` | `load.sq @rb rd` | `op r r` | 3 |
| `$85` | `load.zh` | `load.zh @rb rd` | `op r r` | 3 |
| `$86` | `load.sh` | `load.sh @rb rd` | `op r r` | 3 |
| `$87` | `load` | `load @rb+$disp rd` | `op r r i2` | 5 |
| `$88` | `load.zb` | `load.zb @rb+$disp rd` | `op r r i2` | 5 |
| `$89` | `load.sb` | `load.sb @rb+$disp rd` | `op r r i2` | 5 |
| `$8A` | `load.zq` | `load.zq @rb+$disp rd` | `op r r i2` | 5 |
| `$8B` | `load.sq` | `load.sq @rb+$disp rd` | `op r r i2` | 5 |
| `$8C` | `load.zh` | `load.zh @rb+$disp rd` | `op r r i2` | 5 |
| `$8D` | `load.sh` | `load.sh @rb+$disp rd` | `op r r i2` | 5 |
| `$8E` | `store` | `store rs @rb` | `op r r` | 3 |
| `$8F` | `store.b` | `store.b rs @rb` | `op r r` | 3 |
| `$90` | `store.q` | `store.q rs @rb` | `op r r` | 3 |
| `$91` | `store.h` | `store.h rs @rb` | `op r r` | 3 |
| `$92` | `store` | `store rs @rb+$disp` | `op r r i2` | 5 |
| `$93` | `store.b` | `store.b rs @rb+$disp` | `op r r i2` | 5 |
| `$94` | `store.q` | `store.q rs @rb+$disp` | `op r r i2` | 5 |
| `$95` | `store.h` | `store.h rs @rb+$disp` | `op r r i2` | 5 |
| `$96`..`$9F` | reserved | | | |

## A.9 Extract and insert, `$A0`..`$AF`

The **Slot** column names the operand slot class of each operand byte, as the
instruction-encoding chapter defines it. A sliced slot carries its element index in the
operand byte's form field, and a plain slot requires a form field of `%000`.

| Byte | Mnemonic | Operands | Slots | Form | Len |
|:----:|:---------|:---------|:------|:-----|:---:|
| `$A0` | `extract.zb` | `extract.zb rs.bN rd` | byte-sliced, plain | `op r r` | 3 |
| `$A1` | `extract.sb` | `extract.sb rs.bN rd` | byte-sliced, plain | `op r r` | 3 |
| `$A2` | `extract.zq` | `extract.zq rs.qN rd` | quarter-sliced, plain | `op r r` | 3 |
| `$A3` | `extract.sq` | `extract.sq rs.qN rd` | quarter-sliced, plain | `op r r` | 3 |
| `$A4` | `extract.zh` | `extract.zh rs.hN rd` | half-sliced, plain | `op r r` | 3 |
| `$A5` | `extract.sh` | `extract.sh rs.hN rd` | half-sliced, plain | `op r r` | 3 |
| `$A6` | `insert.b` | `insert.b rs rd.bN` | plain, byte-sliced | `op r r` | 3 |
| `$A7` | `insert.q` | `insert.q rs rd.qN` | plain, quarter-sliced | `op r r` | 3 |
| `$A8` | `insert.h` | `insert.h rs rd.hN` | plain, half-sliced | `op r r` | 3 |
| `$A9` | `bitfield_extract` | `bitfield_extract rs #pos #width rd` | plain, plain | `op r r i1 i1` | 5 |
| `$AA` | `bitfield_extract_signed` | `bitfield_extract_signed rs #pos #width rd` | plain, plain | `op r r i1 i1` | 5 |
| `$AB` | `bitfield_insert` | `bitfield_insert rs #pos #width rd` | plain, plain | `op r r i1 i1` | 5 |
| `$AC`..`$AF` | reserved | | | | |

## A.10 Block memory, `$B0`..`$B7`

| Byte | Mnemonic | Operands | Form | Len |
|:----:|:---------|:---------|:-----|:---:|
| `$B0` | `block_copy` | `block_copy @rs @rd rn` | `op r r r` | 4 |
| `$B1` | `block_copy_forward` | `block_copy_forward @rs @rd rn` | `op r r r` | 4 |
| `$B2` | `block_set` | `block_set rv @rd rn` | `op r r r` | 4 |
| `$B3`..`$B7` | reserved | | | |

An encoding in this band that names the same register in more than one of its three operand
slots raises the illegal-operand trap.

## A.11 System, control registers, TLB, and ports, `$B8`..`$C7`

| Byte | Mnemonic | Operands | Form | Len |
|:----:|:---------|:---------|:-----|:---:|
| `$B8` | `csr_read` | `csr_read $csr rd` | `op r i2` | 4 |
| `$B9` | `csr_write` | `csr_write rs $csr` | `op r i2` | 4 |
| `$BA` | `sys` | `sys #imm` | `op i1` | 2 |
| `$BB` | `sys` | `sys rs` | `op r` | 2 |
| `$BC` | `trap_return` | `trap_return` | `op` | 1 |
| `$BD` | `halt` | `halt` | `op` | 1 |
| `$BE` | `wait_for_interrupt` | `wait_for_interrupt` | `op` | 1 |
| `$BF` | `nop` | `nop` | `op` | 1 |
| `$C0` | `tlb_invalidate_all` | `tlb_invalidate_all` | `op` | 1 |
| `$C1` | `tlb_invalidate_address` | `tlb_invalidate_address rs` | `op r` | 2 |
| `$C2` | `port_in` | `port_in rp rd` | `op r r` | 3 |
| `$C3` | `port_out` | `port_out rs rp` | `op r r` | 3 |
| `$C4`..`$C7` | reserved | | | |

## A.12 Floating point, `$C8`..`$F7`

Within this band the binary64 form of an operation is always at an even offset from `$C8`
and its binary32 `.h` form is the next byte up, so the format bit is the low bit of the
opcode for every paired operation. The two format conversions at `$EA` and `$EB` are the
only members with no pairing, because each names both formats by itself.

| Byte | Mnemonic | Operands | Form | Len |
|:----:|:---------|:---------|:-----|:---:|
| `$C8` | `float_add` | `float_add rs1 rs2 rd` | `op r r r` | 4 |
| `$C9` | `float_add.h` | `float_add.h rs1 rs2 rd` | `op r r r` | 4 |
| `$CA` | `float_subtract` | `float_subtract rs1 rs2 rd` | `op r r r` | 4 |
| `$CB` | `float_subtract.h` | `float_subtract.h rs1 rs2 rd` | `op r r r` | 4 |
| `$CC` | `float_multiply` | `float_multiply rs1 rs2 rd` | `op r r r` | 4 |
| `$CD` | `float_multiply.h` | `float_multiply.h rs1 rs2 rd` | `op r r r` | 4 |
| `$CE` | `float_divide` | `float_divide rs1 rs2 rd` | `op r r r` | 4 |
| `$CF` | `float_divide.h` | `float_divide.h rs1 rs2 rd` | `op r r r` | 4 |
| `$D0` | `float_square_root` | `float_square_root rs rd` | `op r r` | 3 |
| `$D1` | `float_square_root.h` | `float_square_root.h rs rd` | `op r r` | 3 |
| `$D2` | `float_negate` | `float_negate rs rd` | `op r r` | 3 |
| `$D3` | `float_negate.h` | `float_negate.h rs rd` | `op r r` | 3 |
| `$D4` | `float_absolute` | `float_absolute rs rd` | `op r r` | 3 |
| `$D5` | `float_absolute.h` | `float_absolute.h rs rd` | `op r r` | 3 |
| `$D6` | `float_multiply_add` | `float_multiply_add rs1 rs2 rs3 rd` | `op r r r r` | 5 |
| `$D7` | `float_multiply_add.h` | `float_multiply_add.h rs1 rs2 rs3 rd` | `op r r r r` | 5 |
| `$D8` | `float_multiply_subtract` | `float_multiply_subtract rs1 rs2 rs3 rd` | `op r r r r` | 5 |
| `$D9` | `float_multiply_subtract.h` | `float_multiply_subtract.h rs1 rs2 rs3 rd` | `op r r r r` | 5 |
| `$DA` | `float_minimum` | `float_minimum rs1 rs2 rd` | `op r r r` | 4 |
| `$DB` | `float_minimum.h` | `float_minimum.h rs1 rs2 rd` | `op r r r` | 4 |
| `$DC` | `float_maximum` | `float_maximum rs1 rs2 rd` | `op r r r` | 4 |
| `$DD` | `float_maximum.h` | `float_maximum.h rs1 rs2 rd` | `op r r r` | 4 |
| `$DE` | `float_compare_eq` | `float_compare_eq rs1 rs2 rd` | `op r r r` | 4 |
| `$DF` | `float_compare_eq.h` | `float_compare_eq.h rs1 rs2 rd` | `op r r r` | 4 |
| `$E0` | `float_compare_ne` | `float_compare_ne rs1 rs2 rd` | `op r r r` | 4 |
| `$E1` | `float_compare_ne.h` | `float_compare_ne.h rs1 rs2 rd` | `op r r r` | 4 |
| `$E2` | `float_compare_lt` | `float_compare_lt rs1 rs2 rd` | `op r r r` | 4 |
| `$E3` | `float_compare_lt.h` | `float_compare_lt.h rs1 rs2 rd` | `op r r r` | 4 |
| `$E4` | `float_compare_le` | `float_compare_le rs1 rs2 rd` | `op r r r` | 4 |
| `$E5` | `float_compare_le.h` | `float_compare_le.h rs1 rs2 rd` | `op r r r` | 4 |
| `$E6` | `float_compare_ordered` | `float_compare_ordered rs1 rs2 rd` | `op r r r` | 4 |
| `$E7` | `float_compare_ordered.h` | `float_compare_ordered.h rs1 rs2 rd` | `op r r r` | 4 |
| `$E8` | `float_compare_unordered` | `float_compare_unordered rs1 rs2 rd` | `op r r r` | 4 |
| `$E9` | `float_compare_unordered.h` | `float_compare_unordered.h rs1 rs2 rd` | `op r r r` | 4 |
| `$EA` | `float_narrow` | `float_narrow rs rd` | `op r r` | 3 |
| `$EB` | `float_widen` | `float_widen rs rd` | `op r r` | 3 |
| `$EC` | `float_to_signed` | `float_to_signed rs rd` | `op r r` | 3 |
| `$ED` | `float_to_signed.h` | `float_to_signed.h rs rd` | `op r r` | 3 |
| `$EE` | `float_to_unsigned` | `float_to_unsigned rs rd` | `op r r` | 3 |
| `$EF` | `float_to_unsigned.h` | `float_to_unsigned.h rs rd` | `op r r` | 3 |
| `$F0` | `signed_to_float` | `signed_to_float rs rd` | `op r r` | 3 |
| `$F1` | `signed_to_float.h` | `signed_to_float.h rs rd` | `op r r` | 3 |
| `$F2` | `unsigned_to_float` | `unsigned_to_float rs rd` | `op r r` | 3 |
| `$F3` | `unsigned_to_float.h` | `unsigned_to_float.h rs rd` | `op r r` | 3 |
| `$F4`..`$F7` | reserved | | | |

## A.13 Extension escape bytes, `$F8`..`$FE`

Seven bytes open extension opcode pages. Each escape byte, when the machine implements the
extension that owns it, is followed by one opcode byte drawn from that extension's own
256-entry page, and that page's table supplies the length. A machine that does not implement
the owning extension raises the illegal-instruction trap on the escape byte itself and never
fetches the byte after it, which is what makes the absence of an extension observable to a
conformance binary.

| Byte | Role |
|:----:|:-----|
| `$F8` | Escape byte 0, page unallocated |
| `$F9` | Escape byte 1, page unallocated |
| `$FA` | Escape byte 2, page unallocated |
| `$FB` | Escape byte 3, page unallocated |
| `$FC` | Escape byte 4, page unallocated |
| `$FD` | Escape byte 5, page unallocated |
| `$FE` | Escape byte 6, page unallocated |

Allocation of a page to a named extension happens when that extension is ratified, and the
extension registry records the pairing. This appendix fixes the seven bytes and their
mechanism, and it deliberately does not pre-assign pages to the anticipated first
extensions, because an extension that never ships would otherwise hold a page forever.

## A.14 Breakpoint, `$FF`

`$FF` is `breakpoint`, a one-byte trap-class instruction. The byte is pinned here because
`$FF` is the value that fills erased storage, so a run of erased memory reached as code
raises the breakpoint trap at its first byte. Together with the guard at `$00`, both of the
two byte values that a wild jump is most likely to land on stop the machine with a named
cause.

## A.15 Reserved bytes, enumerated

The complete reserved set is `$00`, `$0A`..`$0F`, `$3E`..`$3F`, `$54`..`$5F`, `$6A`..`$6F`,
`$77`..`$7F`, `$96`..`$9F`, `$AC`..`$AF`, `$B3`..`$B7`, `$C4`..`$C7`, and `$F4`..`$F7`,
which is 63 bytes. Every one of them raises the illegal-instruction trap when fetched, and a
conformance binary that executes each of the 63 in turn observes 63 identical traps
differing only in the offending byte and the faulting address.
