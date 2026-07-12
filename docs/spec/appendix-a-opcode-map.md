# Appendix A: Opcode Map (Numeric)

This appendix lists every base-opcode byte `$00`..`$FF` with its mnemonic and operand
shape, in numeric order. It is the encoding-first companion to the function-grouped
Chapter 7. The mode bits (bits 7,6) select the addressing-mode form: `$0x` regVal, `$4x`
immVal, `$8x` regAddr, `$Cx` immAddr, except in the row-packed condition and unary families,
where the two high bits select a row (Chapter 6). `reserved` rows decode as the
illegal-instruction trap when reached as an opcode (cause 0); `$3F`/`$7F`/`$BF` are the
reserved full-byte-dispatch / escape-prefix band and `$FF` is BRK.

The definitive per-byte table is maintained in the repository `README.md` under "Opcodes
Sorted Numerically" and mirrors `src/maize_cpu.h`. The summary below groups the map by base
slot; consult Chapter 7 or Chapter 8 for each instruction's full entry.

## A.1 Base slots `$00`..`$3F` (mode bits select the form)

| Base | Mnemonic | Shape | Forms ($0x/$4x/$8x/$Cx) |
|:----:|:---------|:------|:------------------------|
| `$00` | HALT | zero-op | `$00` (privileged) |
| `$01` | CP / LD | reg/mem | CP `$01` regVal, `$41` immVal; LD `$81` regAddr, `$C1` immAddr |
| `$02` | ST | store | `$02` regVal regAddr, `$42` immVal regAddr |
| `$03` | ADD | ALU | `$03` `$43` `$83` `$C3` |
| `$04` | SUB | ALU | `$04` `$44` `$84` `$C4` |
| `$05` | MUL | ALU | `$05` `$45` `$85` `$C5` |
| `$06` | DIV | ALU | `$06` `$46` `$86` `$C6` |
| `$07` | MOD | ALU | `$07` `$47` `$87` `$C7` |
| `$08` | AND | ALU | `$08` `$48` `$88` `$C8` |
| `$09` | OR | ALU | `$09` `$49` `$89` `$C9` |
| `$0A` | NOR | ALU | `$0A` `$4A` `$8A` `$CA` |
| `$0B` | NAND | ALU | `$0B` `$4B` `$8B` `$CB` |
| `$0C` | XOR | ALU | `$0C` `$4C` `$8C` `$CC` |
| `$0D` | SHL | ALU | `$0D` `$4D` `$8D` `$CD` |
| `$0E` | SHR | ALU | `$0E` `$4E` `$8E` `$CE` |
| `$0F` | CMP | ALU | `$0F` `$4F` `$8F` `$CF` |
| `$10` | TEST | ALU | `$10` `$50` `$90` `$D0` |
| `$11` | CMPXCHG | 3-op | `$11` `$51` `$91` `$D1` |
| `$12` | LEA | 3-op | `$12` `$52` `$92` `$D2` |
| `$13` | CPZ | copy | `$13` regVal, `$53` immVal (`$93`/`$D3` reserved) |
| `$14` | OUT | port | `$14` `$54` `$94` `$D4` (privileged) |
| `$15` | FGETCSR / FSETCSR | FP reg | FGETCSR `$15`, FSETCSR `$55` (`$95`/`$D5` reserved) |
| `$16` | JMP | jump | `$16` `$56` `$96` `$D6` |
| `$17` | Jcc column 0 | branch | JZ `$17`, JB `$57`, JGE `$97`, JAE `$D7` |
| `$18` | Jcc column 1 | branch | JNZ `$18`, JGT `$58`, JLE `$98`, JP `$D8` (`$D9` reserved) |
| `$19` | Jcc column 2 | branch | JLT `$19`, JA `$59`, JBE `$99` |
| `$1A` | FADD | FP ALU | `$1A` `$5A` `$9A` `$DA` |
| `$1B` | FSUB | FP ALU | `$1B` `$5B` `$9B` `$DB` |
| `$1C` | FMUL | FP ALU | `$1C` `$5C` `$9C` `$DC` |
| `$1D` | CALL | call | `$1D` `$5D` `$9D` `$DD` |
| `$1E` | OUTR | port | `$1E` `$5E` `$9E` `$DE` (privileged) |
| `$1F` | IN | port | `$1F` `$5F` `$9F` `$DF` (privileged) |
| `$20` | PUSH | stack | `$20` regVal, `$60` immVal |
| `$21` | FDIV | FP ALU | `$21` `$61` `$A1` `$E1` |
| `$22` | FSQRT / FNEG / FABS | FP unary | `$22` / `$62` / `$A2` (`$E2` reserved) |
| `$23` | FMADD | FP 3-op | `$23` `$63` `$A3` `$E3` |
| `$24` | INT | interrupt | `$24` regVal, `$64` immVal (privileged; dispatch deferred) |
| `$25` | FMSUB | FP 3-op | `$25` `$65` `$A5` `$E5` |
| `$26` | reserved | - | control-register access (reserved) |
| `$27` | RET / IRET / NOP | zero-op | RET `$27`, IRET `$67`, NOP `$A7` (`$E7` reserved) |
| `$28` | reserved | - | paging / MMU control (reserved) |
| `$29` | SETINT / CLRINT / SETCRY / CLRCRY | zero-op | `$29` / `$69` / `$A9` / `$E9` |
| `$2A` | FCMP | FP compare | `$2A` `$6A` `$AA` `$EA` |
| `$2B` | SETcc column 0 | set | SETZ `$2B`, SETB `$6B`, SETGE `$AB`, SETAE `$EB` |
| `$2C` | SETcc column 1 | set | SETNZ `$2C`, SETGT `$6C`, SETLE `$AC`, SETP `$EC` (`$ED` reserved) |
| `$2D` | SETcc column 2 | set | SETLT `$2D`, SETA `$6D`, SETBE `$AD` |
| `$2E` | SAR | ALU | `$2E` `$6E` `$AE` `$EE` |
| `$2F` | CMPIND | compare-ind | `$2F` regVal regAddr, `$6F` immVal regAddr |
| `$30` | TSTIND | test-ind | `$30` regVal regAddr, `$70` immVal regAddr |
| `$31` | INC / DEC / NOT / NEG | unary | INC `$31`, DEC `$71`, NOT `$B1`, NEG `$F1` |
| `$32` | CLR / POP | unary | CLR `$32`, POP `$72` (`$B2`/`$F2` reserved) |
| `$33` | FMIN / FMAX | FP min/max | FMIN `$33`, FMAX `$73` (`$B3`/`$F3` reserved) |
| `$34` | SYS | syscall | `$34` regVal, `$74` immVal (privileged) |
| `$35` | UDIV | ALU | `$35` `$75` `$B5` `$F5` |
| `$36` | UMOD | ALU | `$36` `$76` `$B6` `$F6` |
| `$37` | reserved | - | SMP / memory-ordering primitives (reserved) |
| `$38` | reserved | - | versioning / capability query (reserved) |
| `$39` | FCVTFF / FCVTFS / FCVTFU | FP convert | `$39` / `$79` / `$B9` (`$F9` reserved) |
| `$3A` | FCVTSF / FCVTUF | FP convert | `$3A` / `$7A` (`$BA`/`$FA` reserved) |
| `$3B` | ADC | ALU | `$3B` `$7B` `$BB` `$FB` |
| `$3C` | SBB | ALU | `$3C` `$7C` `$BC` `$FC` |
| `$3D` | MULW | 3-op | `$3D` `$7D` `$BD` `$FD` |
| `$3E` | UMULW | 3-op | `$3E` `$7E` `$BE` `$FE` |
| `$3F` | reserved | - | full-byte-dispatch / escape-prefix band (`$3F`/`$7F`/`$BF`) |

## A.2 The standalone high-byte encodings

A few instructions occupy a single fixed byte rather than a base-slot family:

| Byte | Mnemonic | Shape |
|:----:|:---------|:------|
| `$E0` | XCHG | reg reg |
| `$E4` | reserved | - |
| `$FF` | BRK | zero-op (breakpoint trap, cause 3) |

## A.3 Reserved bytes

Every byte not assigned above decodes as `reserved`. Reached as an opcode, a reserved byte
raises the illegal-instruction trap (cause 0, subcode 0); an unallocated condition encoding
in the Jcc / SETcc families raises cause 0 subcode 1. The full byte-by-byte enumeration,
including each reserved row's earmark, is the repository README "Opcodes Sorted Numerically"
table and the Reserved Space chapter.
