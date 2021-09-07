; This file contains the code for the kernel

LABEL core_os_core                     AUTO
LABEL core_os_key_input                AUTO
LABEL core_os_print_reg                AUTO
LABEL core_os_puts                     AUTO
LABEL core_os_putchar                  AUTO
LABEL core_os_shut_down                AUTO
LABEL core_os_strlen                   AUTO

LABEL core_os_exception_handler        AUTO
LABEL core_os_exception_handler_00     AUTO
LABEL core_os_exception_handler_01     AUTO
LABEL core_os_exception_handler_02     AUTO
LABEL core_os_exception_handler_03     AUTO
LABEL core_os_exception_handler_04     AUTO
LABEL core_os_exception_handler_05     AUTO
LABEL core_os_exception_handler_06     AUTO
LABEL core_os_exception_handler_07     AUTO
LABEL core_os_exception_handler_08     AUTO
LABEL core_os_exception_handler_09     AUTO
LABEL core_os_exception_handler_0A     AUTO
LABEL core_os_exception_handler_0B     AUTO
LABEL core_os_exception_handler_0C     AUTO
LABEL core_os_exception_handler_0D     AUTO
LABEL core_os_exception_handler_0E     AUTO
LABEL core_os_exception_handler_0F     AUTO

LABEL core_bios_10                     AUTO
LABEL core_bios_16                     AUTO
LABEL core_syscall                     AUTO

LABEL core_sys_write                   AUTO    ; $01 #1
LABEL core_os_hex_print_b0             AUTO
LABEL core_os_hex_print_b1             AUTO
LABEL core_os_hex_print_b2             AUTO
LABEL core_os_hex_print_b3             AUTO
LABEL core_os_hex_print_b4             AUTO
LABEL core_os_hex_print_b5             AUTO
LABEL core_os_hex_print_b6             AUTO
LABEL core_os_hex_print_b7             AUTO
LABEL core_os_hex_print_q0             AUTO
LABEL core_os_hex_print_q1             AUTO
LABEL core_os_hex_print_q2             AUTO
LABEL core_os_hex_print_q3             AUTO
LABEL core_os_hex_print_h0             AUTO
LABEL core_os_hex_print_h1             AUTO
LABEL core_os_hex_print_w0             AUTO
LABEL core_os_print_newline            AUTO


;******************************************************************************
; Interrupt vectors begin at segment $0000`0000 address $0000`0000.

$00000000:
   ADDRESS core_os_exception_handler_00   ; INT 00 divide by zero
   ADDRESS core_os_exception_handler_01   ; INT 01 
   ADDRESS core_os_exception_handler_02   ; INT 02 
   ADDRESS core_os_exception_handler_03   ; INT 03 
   ADDRESS core_os_exception_handler_04   ; INT 04
   ADDRESS core_os_exception_handler_05   ; INT 05 
   ADDRESS core_os_exception_handler_06   ; INT 06 invalid opcode
   ADDRESS core_os_exception_handler_07   ; INT 07 
   ADDRESS core_os_exception_handler_08   ; INT 08 
   ADDRESS core_os_exception_handler_09   ; INT 09 
   ADDRESS core_os_exception_handler_0A   ; INT 0A 
   ADDRESS core_os_exception_handler_0B   ; INT 0B 
   ADDRESS core_os_exception_handler_0C   ; INT 0C 
   ADDRESS core_os_exception_handler_0D   ; INT 0D 
   ADDRESS core_os_exception_handler_0E   ; INT 0E 
   ADDRESS core_os_exception_handler_0F   ; INT 0F 
   ADDRESS core_bios_10                   ; INT 10 Classic BIOS
   ADDRESS $0000`0000                     ; INT 11 
   ADDRESS $0000`0000                     ; INT 12 
   ADDRESS $0000`0000                     ; INT 13 
   ADDRESS $0000`0000                     ; INT 14 
   ADDRESS $0000`0000                     ; INT 15 
   ADDRESS core_bios_16                   ; INT 16 Classic BIOS
   ADDRESS $0000`0000                     ; INT 17 
   ADDRESS $0000`0000                     ; INT 18 
   ADDRESS $0000`0000                     ; INT 19 
   ADDRESS $0000`0000                     ; INT 1A 
   ADDRESS $0000`0000                     ; INT 1B 
   ADDRESS $0000`0000                     ; INT 1C 
   ADDRESS $0000`0000                     ; INT 1D 
   ADDRESS $0000`0000                     ; INT 1E 
   ADDRESS $0000`0000                     ; INT 1F 
   ADDRESS $0000`0000                     ; INT 20 
   ADDRESS core_os_key_input              ; INT 21 
   ADDRESS $0000`0000                     ; INT 22 
   ADDRESS $0000`0000                     ; INT 23 
   ADDRESS $0000`0000                     ; INT 24 
   ADDRESS $0000`0000                     ; INT 25 
   ADDRESS $0000`0000                     ; INT 26 
   ADDRESS $0000`0000                     ; INT 27 
   ADDRESS $0000`0000                     ; INT 28 
   ADDRESS $0000`0000                     ; INT 29 
   ADDRESS $0000`0000                     ; INT 2A 
   ADDRESS $0000`0000                     ; INT 2B 
   ADDRESS $0000`0000                     ; INT 2C 
   ADDRESS $0000`0000                     ; INT 2D 
   ADDRESS $0000`0000                     ; INT 2E 
   ADDRESS $0000`0000                     ; INT 2F 
   ADDRESS $0000`0000                     ; INT 30 
   ADDRESS $0000`0000                     ; INT 31 
   ADDRESS $0000`0000                     ; INT 32 
   ADDRESS $0000`0000                     ; INT 33 
   ADDRESS $0000`0000                     ; INT 34 
   ADDRESS $0000`0000                     ; INT 35 
   ADDRESS $0000`0000                     ; INT 36 
   ADDRESS $0000`0000                     ; INT 37 
   ADDRESS $0000`0000                     ; INT 38 
   ADDRESS $0000`0000                     ; INT 39 
   ADDRESS $0000`0000                     ; INT 3A 
   ADDRESS $0000`0000                     ; INT 3B 
   ADDRESS $0000`0000                     ; INT 3C 
   ADDRESS $0000`0000                     ; INT 3D 
   ADDRESS $0000`0000                     ; INT 3E 
   ADDRESS $0000`0000                     ; INT 3F 
   ADDRESS core_os_core                   ; INT 40 
   ADDRESS $0000`0000                     ; INT 41 
   ADDRESS $0000`0000                     ; INT 42 
   ADDRESS $0000`0000                     ; INT 43 
   ADDRESS $0000`0000                     ; INT 44 
   ADDRESS $0000`0000                     ; INT 45 
   ADDRESS $0000`0000                     ; INT 46 
   ADDRESS $0000`0000                     ; INT 47 
   ADDRESS $0000`0000                     ; INT 48 
   ADDRESS $0000`0000                     ; INT 49 
   ADDRESS $0000`0000                     ; INT 4A 
   ADDRESS $0000`0000                     ; INT 4B 
   ADDRESS $0000`0000                     ; INT 4C 
   ADDRESS $0000`0000                     ; INT 4D 
   ADDRESS $0000`0000                     ; INT 4E 
   ADDRESS $0000`0000                     ; INT 4F 
   ADDRESS $0000`0000                     ; INT 50 
   ADDRESS $0000`0000                     ; INT 51 
   ADDRESS $0000`0000                     ; INT 52 
   ADDRESS $0000`0000                     ; INT 53 
   ADDRESS $0000`0000                     ; INT 54 
   ADDRESS $0000`0000                     ; INT 55 
   ADDRESS $0000`0000                     ; INT 56 
   ADDRESS $0000`0000                     ; INT 57 
   ADDRESS $0000`0000                     ; INT 58 
   ADDRESS $0000`0000                     ; INT 59 
   ADDRESS $0000`0000                     ; INT 5A 
   ADDRESS $0000`0000                     ; INT 5B 
   ADDRESS $0000`0000                     ; INT 5C 
   ADDRESS $0000`0000                     ; INT 5D 
   ADDRESS $0000`0000                     ; INT 5E 
   ADDRESS $0000`0000                     ; INT 5F 
   ADDRESS $0000`0000                     ; INT 60 
   ADDRESS $0000`0000                     ; INT 61 
   ADDRESS $0000`0000                     ; INT 62 
   ADDRESS $0000`0000                     ; INT 63 
   ADDRESS $0000`0000                     ; INT 64 
   ADDRESS $0000`0000                     ; INT 65 
   ADDRESS $0000`0000                     ; INT 66 
   ADDRESS $0000`0000                     ; INT 67 
   ADDRESS $0000`0000                     ; INT 68 
   ADDRESS $0000`0000                     ; INT 69 
   ADDRESS $0000`0000                     ; INT 6A 
   ADDRESS $0000`0000                     ; INT 6B 
   ADDRESS $0000`0000                     ; INT 6C 
   ADDRESS $0000`0000                     ; INT 6D 
   ADDRESS $0000`0000                     ; INT 6E 
   ADDRESS $0000`0000                     ; INT 6F 
   ADDRESS $0000`0000                     ; INT 70 
   ADDRESS $0000`0000                     ; INT 71 
   ADDRESS $0000`0000                     ; INT 72 
   ADDRESS $0000`0000                     ; INT 73 
   ADDRESS $0000`0000                     ; INT 74 
   ADDRESS $0000`0000                     ; INT 75 
   ADDRESS $0000`0000                     ; INT 76 
   ADDRESS $0000`0000                     ; INT 77 
   ADDRESS $0000`0000                     ; INT 78 
   ADDRESS $0000`0000                     ; INT 79 
   ADDRESS $0000`0000                     ; INT 7A 
   ADDRESS $0000`0000                     ; INT 7B 
   ADDRESS $0000`0000                     ; INT 7C 
   ADDRESS $0000`0000                     ; INT 7D 
   ADDRESS $0000`0000                     ; INT 7E 
   ADDRESS $0000`0000                     ; INT 7F 
   ADDRESS core_syscall                   ; INT 80


;******************************************************************************
;******************************************************************************
; BIOS $10 implementation

LABEL core_bios_10_jump_table             AUTO
LABEL core_bios_10_nop                    AUTO
LABEL core_bios_10_clear_screen           AUTO
LABEL core_bios_10_set_video_mode         AUTO    
LABEL core_bios_10_set_cursor_shape       AUTO    
LABEL core_bios_10_set_cursor_pos         AUTO    
LABEL core_bios_10_get_cursor_pos         AUTO    
LABEL core_bios_10_scroll_up              AUTO    
LABEL core_bios_10_scroll_down            AUTO    
LABEL core_bios_10_read_char_and_attr     AUTO 
LABEL core_bios_10_write_char_and_attr    AUTO 
LABEL core_bios_10_write_char             AUTO
LABEL core_bios_10_set_color              AUTO


;******************************************************************************
; This is the entry point for all BIOS $10 calls. It indexes into the jump 
; table using the BIOS opcode and forwards the call to the correct function.

core_bios_10:
   PUSH B
   CLR B
   LD A.B1 B.H0  ; The opcode is in A.B1, so copy it over to B.H0 so we can operate on it.
   MUL $04 B.H0  ; Multiply the opcode by 4 to find its offset in the jump table.
   LD core_bios_10_jump_table B.H1
   ADD B.H0 B.H1  ; Add the jump table start address and the calculated offset, and we 
                  ; get the pointer to the operation's implementing function.
   CALL @B.H1
   POP B
   IRET


;******************************************************************************
; BIOS $10 jump table that translates AH opcodes into functions.

core_bios_10_jump_table:
   ADDRESS core_bios_10_set_video_mode      ; AH = $00
   ADDRESS core_bios_10_set_cursor_shape    ; AH = $01
   ADDRESS core_bios_10_set_cursor_pos      ; AH = $02
   ADDRESS core_bios_10_get_cursor_pos      ; AH = $03
   ADDRESS core_bios_10_clear_screen        ; AH = $04
   ADDRESS core_bios_10_nop                 ; AH = $05
   ADDRESS core_bios_10_scroll_up           ; AH = $06
   ADDRESS core_bios_10_scroll_down         ; AH = $07
   ADDRESS core_bios_10_read_char_and_attr  ; AH = $08
   ADDRESS core_bios_10_write_char_and_attr ; AH = $09
   ADDRESS core_bios_10_write_char          ; AH = $0A
   ADDRESS core_bios_10_set_color           ; AH = $0B


core_bios_10_set_video_mode:
   OUT A $7F
   RET


core_bios_10_set_cursor_shape:       
   OUT A $7F
   RET
         

core_bios_10_set_cursor_pos:         
   OUT A $7F
   RET
         

core_bios_10_get_cursor_pos:
   PUSH B
   CLR B
   IN $7F B
   POP B
   RET
         

core_bios_10_nop:                    
   RET
         

core_bios_10_scroll_up:              
   OUT A $7F
   RET
         

core_bios_10_scroll_down:            
   OUT A $7F
   RET
         

core_bios_10_read_char_and_attr:     
   RET
         

core_bios_10_write_char_and_attr:    
   OUT A $7F
   RET


core_bios_10_write_char:
   OUT A $7F
   RET


core_bios_10_clear_screen:
   OUT A $7F
   RET


core_bios_10_set_color:
   OUT A $7F
   RET


;******************************************************************************
;******************************************************************************
; BIOS $16 implementation

LABEL core_bios_16_jump_table                AUTO
LABEL core_bios_16_get_keystroke             AUTO
LABEL core_bios_16_check_for_keystroke       AUTO
LABEL core_bios_16_get_shift_flags           AUTO
LABEL core_bios_16_set_rate_delay            AUTO
LABEL core_bios_16_get_enh_keystroke         AUTO
LABEL core_bios_16_check_enh_keystroke       AUTO
LABEL core_bios_16_get_ext_shift_states      AUTO


;******************************************************************************
; BIOS $10 entry point

core_bios_16:
   PUSH B
   CLR B
   LD A.B1 B.H0
   MUL $04 B.H0
   LD core_bios_16_jump_table B.H1
   ADD B.H0 B.H1
   CALL @B.H1
   POP B
   IRET


;******************************************************************************
; BIOS $16 jump table

core_bios_16_jump_table:
   ADDRESS core_bios_16_get_keystroke        ; AH = $00
   ADDRESS core_bios_16_check_for_keystroke  ; AH = $01
   ADDRESS core_bios_16_get_shift_flags      ; AH = $02
   ADDRESS core_bios_16_set_rate_delay       ; AH = $03
   ADDRESS core_bios_16_nop                  ; AH = $04
   ADDRESS core_bios_16_nop                  ; AH = $05
   ADDRESS core_bios_16_nop                  ; AH = $06
   ADDRESS core_bios_16_nop                  ; AH = $07
   ADDRESS core_bios_16_nop                  ; AH = $08
   ADDRESS core_bios_16_nop                  ; AH = $09
   ADDRESS core_bios_16_nop                  ; AH = $0A
   ADDRESS core_bios_16_nop                  ; AH = $0B
   ADDRESS core_bios_16_nop                  ; AH = $0C
   ADDRESS core_bios_16_nop                  ; AH = $0D
   ADDRESS core_bios_16_nop                  ; AH = $0E
   ADDRESS core_bios_16_nop                  ; AH = $0F
   ADDRESS core_bios_16_nop                  ; AH = $10
   ADDRESS core_bios_16_nop                  ; AH = $11
   ADDRESS core_bios_16_nop                  ; AH = $12


core_bios_16_nop:                    
   RET
         

core_bios_16_get_keystroke:
   PUSH B
   CLR B
   IN $60 B
   LD B.Q0 A.Q0 ; character
   POP B
   RET


core_bios_16_check_for_keystroke:
   RET


core_bios_16_get_shift_flags:
   RET


core_bios_16_set_rate_delay:
   RET


;******************************************************************************
;******************************************************************************
; Core OS functions that aren't necessarily in the system calls table.

LABEL core_os_core_jump_table          AUTO

core_os_core:
   PUSH B
   CLR B
   LD A.Q0 B.H0
   MUL $04 B.H0
   LD core_os_core_jump_table B.H1
   ADD B.H0 B.H1
   CALL @B.H1
   POP B
   IRET

LABEL core_os_idle                     AUTO
LABEL core_os_nop                      AUTO


;******************************************************************************
; The core_os_core function uses this jump table to vector to OS functions.

core_os_core_jump_table:
   ADDRESS core_os_shut_down           ; A.Q0 = $0000
   ADDRESS core_os_idle                ; A.Q1 = $0001
   ADDRESS core_os_puts                ; A.Q0 = $0002
   ADDRESS core_os_putchar             ; A.Q0 = $0003


;******************************************************************************
; "Power down" the virtual CPU.

core_os_shut_down:
   CLR A
   OUT A $0001
   RET


;******************************************************************************
; The idle routine is executed when the OS isn't doing any work. HALT stops the 
; clock, and any interrupt will wake the CPU, jump to the interrupt handler, 
; and return to the JMP instruction, which goes right back to HALT.

LABEL core_os_idle_halt AUTO

core_os_idle:
   ADD $08 S.H0 ; This routine never returns, so adjust the stack back above the return address

core_os_idle_halt:
   HALT
   JMP core_os_idle_halt


;******************************************************************************
; Output a string to the console.

LABEL core_os_puts_exit AUTO
LABEL core_os_puts_next_char AUTO

core_os_puts:
   PUSH G
   PUSH H
   PUSH J
   LD G.H0 H.H0
   CLR J
core_os_puts_next_char:
   CMPIND $00 @G.H0
   JZ core_os_puts_exit
   INC J
   INC G.H0
   JMP core_os_puts_next_char
core_os_puts_exit:
   CLR A
   LD $01 G
   CALL core_sys_write
   POP J
   POP H
   POP G
   LD G.H0 A.H0
   RET


;******************************************************************************
; Output a character to the console.

core_os_putchar:
   ; Put the character we want to output onto the stack.
   PUSH G
   PUSH H
   PUSH J
   PUSH G.B0

   ; Now S.H0 points at that character on the stack, so copy that address to 
   ; H.H0, which is the output pointer (const char *).
   LD S.H0 H.H0
   
   ; Delegate output to the sys_write call.
   LD $01 G ; STDOUT file descriptor
   LD $01 J ; Number of characters to write, starting at @H.H0
   CALL core_sys_write
   
   POP A.B0
   POP J
   POP H
   POP G
   RET


;******************************************************************************
; Echo routine

core_os_key_input:
   PUSH A
   CLR A
   INT $16 ; get keystroke
   LD $01 A.B5 ; Count
   LD $0A A.B1 ; Function to execute
   INT $10     ; BIOS interrupt
   POP A
   IRET


;******************************************************************************
; Return length of a null-terminated string.

LABEL core_os_strlen_loop AUTO
LABEL core_os_strlen_done AUTO

core_os_strlen:
   PUSH G.H0
   CLR A
core_os_strlen_loop:
   CMPIND $00 @G.H0
   JZ core_os_strlen_done
   INC A.H0
   INC G.H0
   JMP core_os_strlen_loop
core_os_strlen_done:
   POP G.H0
   RET


;******************************************************************************
; Linux syscalls are implemented here.

LABEL core_syscall_jump_table       AUTO
LABEL core_sys_nop                  AUTO
LABEL core_sys_read                 AUTO    ; $00 #0
LABEL core_sys_exit                 AUTO    ; $3C #60
LABEL core_sys_reboot               AUTO    ; $A9 #169

core_syscall:
   MUL $04 A.H0
   LD core_syscall_jump_table A.H1
   ADD A.H0 A.H1
   CALL @A.H1
   IRET

core_syscall_jump_table:
   ADDRESS core_sys_read            ; $00 #0
   ADDRESS core_sys_write           ; $01 #1
   ADDRESS core_sys_nop             ; $02 #
   ADDRESS core_sys_nop             ; $03 #
   ADDRESS core_sys_nop             ; $04 #
   ADDRESS core_sys_nop             ; $05 #
   ADDRESS core_sys_nop             ; $06 #
   ADDRESS core_sys_nop             ; $07 #
   ADDRESS core_sys_nop             ; $08 #
   ADDRESS core_sys_nop             ; $09 #
   ADDRESS core_sys_nop             ; $0A #
   ADDRESS core_sys_nop             ; $0B #
   ADDRESS core_sys_nop             ; $0C #
   ADDRESS core_sys_nop             ; $0D #
   ADDRESS core_sys_nop             ; $0E #
   ADDRESS core_sys_nop             ; $0F #
   ADDRESS core_sys_nop             ; $10 #
   ADDRESS core_sys_nop             ; $11 #
   ADDRESS core_sys_nop             ; $12 #
   ADDRESS core_sys_nop             ; $13 #
   ADDRESS core_sys_nop             ; $14 #
   ADDRESS core_sys_nop             ; $15 #
   ADDRESS core_sys_nop             ; $16 #
   ADDRESS core_sys_nop             ; $17 #
   ADDRESS core_sys_nop             ; $18 #
   ADDRESS core_sys_nop             ; $19 #
   ADDRESS core_sys_nop             ; $1A #
   ADDRESS core_sys_nop             ; $1B #
   ADDRESS core_sys_nop             ; $1C #
   ADDRESS core_sys_nop             ; $1D #
   ADDRESS core_sys_nop             ; $1E #
   ADDRESS core_sys_nop             ; $1F #
   ADDRESS core_sys_nop             ; $20 #
   ADDRESS core_sys_nop             ; $21 #
   ADDRESS core_sys_nop             ; $22 #
   ADDRESS core_sys_nop             ; $23 #
   ADDRESS core_sys_nop             ; $24 #
   ADDRESS core_sys_nop             ; $25 #
   ADDRESS core_sys_nop             ; $26 #
   ADDRESS core_sys_nop             ; $27 #
   ADDRESS core_sys_nop             ; $28 #
   ADDRESS core_sys_nop             ; $29 #
   ADDRESS core_sys_nop             ; $2A #
   ADDRESS core_sys_nop             ; $2B #
   ADDRESS core_sys_nop             ; $2C #
   ADDRESS core_sys_nop             ; $2D #
   ADDRESS core_sys_nop             ; $2E #
   ADDRESS core_sys_nop             ; $2F #
   ADDRESS core_sys_nop             ; $30 #
   ADDRESS core_sys_nop             ; $31 #
   ADDRESS core_sys_nop             ; $32 #
   ADDRESS core_sys_nop             ; $33 #
   ADDRESS core_sys_nop             ; $34 #
   ADDRESS core_sys_nop             ; $35 #
   ADDRESS core_sys_nop             ; $36 #
   ADDRESS core_sys_nop             ; $37 #
   ADDRESS core_sys_nop             ; $38 #
   ADDRESS core_sys_nop             ; $39 #
   ADDRESS core_sys_nop             ; $3A #
   ADDRESS core_sys_nop             ; $3B #
   ADDRESS core_sys_exit            ; $3C #60
   ADDRESS core_sys_nop             ; $3D #
   ADDRESS core_sys_nop             ; $3E #
   ADDRESS core_sys_nop             ; $3F #
   ADDRESS core_sys_nop             ; $41 #
   ADDRESS core_sys_nop             ; $42 #
   ADDRESS core_sys_nop             ; $43 #
   ADDRESS core_sys_nop             ; $44 #
   ADDRESS core_sys_nop             ; $45 #
   ADDRESS core_sys_nop             ; $46 #
   ADDRESS core_sys_nop             ; $47 #
   ADDRESS core_sys_nop             ; $48 #
   ADDRESS core_sys_nop             ; $49 #
   ADDRESS core_sys_nop             ; $4A #
   ADDRESS core_sys_nop             ; $4B #
   ADDRESS core_sys_nop             ; $4C #
   ADDRESS core_sys_nop             ; $40 #
   ADDRESS core_sys_nop             ; $4D #
   ADDRESS core_sys_nop             ; $4E #
   ADDRESS core_sys_nop             ; $4F #
   ADDRESS core_sys_nop             ; $50 #
   ADDRESS core_sys_nop             ; $51 #
   ADDRESS core_sys_nop             ; $52 #
   ADDRESS core_sys_nop             ; $53 #
   ADDRESS core_sys_nop             ; $54 #
   ADDRESS core_sys_nop             ; $55 #
   ADDRESS core_sys_nop             ; $56 #
   ADDRESS core_sys_nop             ; $57 #
   ADDRESS core_sys_nop             ; $58 #
   ADDRESS core_sys_nop             ; $59 #
   ADDRESS core_sys_nop             ; $5A #
   ADDRESS core_sys_nop             ; $5B #
   ADDRESS core_sys_nop             ; $5C #
   ADDRESS core_sys_nop             ; $5D #
   ADDRESS core_sys_nop             ; $5E #
   ADDRESS core_sys_nop             ; $5F #
   ADDRESS core_sys_nop             ; $60 #
   ADDRESS core_sys_nop             ; $61 #
   ADDRESS core_sys_nop             ; $62 #
   ADDRESS core_sys_nop             ; $63 #
   ADDRESS core_sys_nop             ; $64 #
   ADDRESS core_sys_nop             ; $65 #
   ADDRESS core_sys_nop             ; $66 #
   ADDRESS core_sys_nop             ; $67 #
   ADDRESS core_sys_nop             ; $68 #
   ADDRESS core_sys_nop             ; $69 #
   ADDRESS core_sys_nop             ; $6A #
   ADDRESS core_sys_nop             ; $6B #
   ADDRESS core_sys_nop             ; $6C #
   ADDRESS core_sys_nop             ; $6D #
   ADDRESS core_sys_nop             ; $6E #
   ADDRESS core_sys_nop             ; $6F #
   ADDRESS core_sys_nop             ; $70 #
   ADDRESS core_sys_nop             ; $71 #
   ADDRESS core_sys_nop             ; $72 #
   ADDRESS core_sys_nop             ; $73 #
   ADDRESS core_sys_nop             ; $74 #
   ADDRESS core_sys_nop             ; $75 #
   ADDRESS core_sys_nop             ; $76 #
   ADDRESS core_sys_nop             ; $77 #
   ADDRESS core_sys_nop             ; $78 #
   ADDRESS core_sys_nop             ; $79 #
   ADDRESS core_sys_nop             ; $7A #
   ADDRESS core_sys_nop             ; $7B #
   ADDRESS core_sys_nop             ; $7C #
   ADDRESS core_sys_nop             ; $7D #
   ADDRESS core_sys_nop             ; $7E #
   ADDRESS core_sys_nop             ; $7F #
   ADDRESS core_sys_nop             ; $80 #
   ADDRESS core_sys_nop             ; $81 #
   ADDRESS core_sys_nop             ; $82 #
   ADDRESS core_sys_nop             ; $83 #
   ADDRESS core_sys_nop             ; $84 #
   ADDRESS core_sys_nop             ; $85 #
   ADDRESS core_sys_nop             ; $86 #
   ADDRESS core_sys_nop             ; $87 #
   ADDRESS core_sys_nop             ; $88 #
   ADDRESS core_sys_nop             ; $89 #
   ADDRESS core_sys_nop             ; $8A #
   ADDRESS core_sys_nop             ; $8B #
   ADDRESS core_sys_nop             ; $8C #
   ADDRESS core_sys_nop             ; $8D #
   ADDRESS core_sys_nop             ; $8E #
   ADDRESS core_sys_nop             ; $8F #
   ADDRESS core_sys_nop             ; $90 #
   ADDRESS core_sys_nop             ; $91 #
   ADDRESS core_sys_nop             ; $92 #
   ADDRESS core_sys_nop             ; $93 #
   ADDRESS core_sys_nop             ; $94 #
   ADDRESS core_sys_nop             ; $95 #
   ADDRESS core_sys_nop             ; $96 #
   ADDRESS core_sys_nop             ; $97 #
   ADDRESS core_sys_nop             ; $98 #
   ADDRESS core_sys_nop             ; $99 #
   ADDRESS core_sys_nop             ; $9A #
   ADDRESS core_sys_nop             ; $9B #
   ADDRESS core_sys_nop             ; $9C #
   ADDRESS core_sys_nop             ; $9D #
   ADDRESS core_sys_nop             ; $9E #
   ADDRESS core_sys_nop             ; $9F #
   ADDRESS core_sys_nop             ; $A0 #
   ADDRESS core_sys_nop             ; $A1 #
   ADDRESS core_sys_nop             ; $A2 #
   ADDRESS core_sys_nop             ; $A3 #
   ADDRESS core_sys_nop             ; $A4 #
   ADDRESS core_sys_nop             ; $A5 #
   ADDRESS core_sys_nop             ; $A6 #
   ADDRESS core_sys_nop             ; $A7 #
   ADDRESS core_sys_nop             ; $A8 #
   ADDRESS core_sys_reboot          ; $A9 #169
   ADDRESS core_sys_nop             ; $AA #
   ADDRESS core_sys_nop             ; $AB #
   ADDRESS core_sys_nop             ; $AC #
   ADDRESS core_sys_nop             ; $AD #
   ADDRESS core_sys_nop             ; $AE #
   ADDRESS core_sys_nop             ; $AF #
   ADDRESS core_sys_nop             ; $B0 #
   ADDRESS core_sys_nop             ; $B1 #
   ADDRESS core_sys_nop             ; $B2 #
   ADDRESS core_sys_nop             ; $B3 #
   ADDRESS core_sys_nop             ; $B4 #
   ADDRESS core_sys_nop             ; $B5 #
   ADDRESS core_sys_nop             ; $B6 #
   ADDRESS core_sys_nop             ; $B7 #
   ADDRESS core_sys_nop             ; $B8 #
   ADDRESS core_sys_nop             ; $B9 #
   ADDRESS core_sys_nop             ; $BA #
   ADDRESS core_sys_nop             ; $BB #
   ADDRESS core_sys_nop             ; $BC #
   ADDRESS core_sys_nop             ; $BD #
   ADDRESS core_sys_nop             ; $BE #
   ADDRESS core_sys_nop             ; $BF #
   ADDRESS core_sys_nop             ; $C0 #
   ADDRESS core_sys_nop             ; $C1 #
   ADDRESS core_sys_nop             ; $C2 #
   ADDRESS core_sys_nop             ; $C3 #
   ADDRESS core_sys_nop             ; $C4 #
   ADDRESS core_sys_nop             ; $C5 #
   ADDRESS core_sys_nop             ; $C6 #
   ADDRESS core_sys_nop             ; $C7 #
   ADDRESS core_sys_nop             ; $C8 #
   ADDRESS core_sys_nop             ; $C9 #
   ADDRESS core_sys_nop             ; $CA #
   ADDRESS core_sys_nop             ; $CB #
   ADDRESS core_sys_nop             ; $CC #
   ADDRESS core_sys_nop             ; $CD #
   ADDRESS core_sys_nop             ; $CE #
   ADDRESS core_sys_nop             ; $CF #
   ADDRESS core_sys_nop             ; $D0 #
   ADDRESS core_sys_nop             ; $D1 #
   ADDRESS core_sys_nop             ; $D2 #
   ADDRESS core_sys_nop             ; $D3 #
   ADDRESS core_sys_nop             ; $D4 #
   ADDRESS core_sys_nop             ; $D5 #
   ADDRESS core_sys_nop             ; $D6 #
   ADDRESS core_sys_nop             ; $D7 #
   ADDRESS core_sys_nop             ; $D8 #
   ADDRESS core_sys_nop             ; $D9 #
   ADDRESS core_sys_nop             ; $DA #
   ADDRESS core_sys_nop             ; $DB #
   ADDRESS core_sys_nop             ; $DC #
   ADDRESS core_sys_nop             ; $DD #
   ADDRESS core_sys_nop             ; $DE #
   ADDRESS core_sys_nop             ; $DF #
   ADDRESS core_sys_nop             ; $E0 #
   ADDRESS core_sys_nop             ; $E1 #
   ADDRESS core_sys_nop             ; $E2 #
   ADDRESS core_sys_nop             ; $E3 #
   ADDRESS core_sys_nop             ; $E4 #
   ADDRESS core_sys_nop             ; $E5 #
   ADDRESS core_sys_nop             ; $E6 #
   ADDRESS core_sys_nop             ; $E7 #
   ADDRESS core_sys_nop             ; $E8 #
   ADDRESS core_sys_nop             ; $E9 #
   ADDRESS core_sys_nop             ; $EA #
   ADDRESS core_sys_nop             ; $EB #
   ADDRESS core_sys_nop             ; $EC #
   ADDRESS core_sys_nop             ; $ED #
   ADDRESS core_sys_nop             ; $EE #
   ADDRESS core_sys_nop             ; $EF #
   ADDRESS core_sys_nop             ; $F0 #
   ADDRESS core_sys_nop             ; $F1 #
   ADDRESS core_sys_nop             ; $F2 #
   ADDRESS core_sys_nop             ; $F3 #
   ADDRESS core_sys_nop             ; $F4 #
   ADDRESS core_sys_nop             ; $F5 #
   ADDRESS core_sys_nop             ; $F6 #
   ADDRESS core_sys_nop             ; $F7 #
   ADDRESS core_sys_nop             ; $F8 #
   ADDRESS core_sys_nop             ; $F9 #
   ADDRESS core_sys_nop             ; $FA #
   ADDRESS core_sys_nop             ; $FB #
   ADDRESS core_sys_nop             ; $FC #
   ADDRESS core_sys_nop             ; $FD #
   ADDRESS core_sys_nop             ; $FE #
   ADDRESS core_sys_nop             ; $FF #


;******************************************************************************
; Placeholder for system calls that haven't been implemented yet, but for which 
; I want to reserve a place.

core_sys_nop:
   RET


;******************************************************************************
; sys_read system call

core_sys_read:
   RET


;******************************************************************************
; sys_write system call
; 
; PARAMETERS
; G unsigned int fd
; H const char *buf
; J size_t count
; 
; RETURN
; A size_t number of bytes written

LABEL core_sys_write_exit AUTO
LABEL core_sys_write_next_char AUTO

core_sys_write:
   PUSH B
   PUSH Z.H0
   CLR A
   CLR B
   CLR Z.H0
   LD $0A B.B1
core_sys_write_next_char:
   CMP Z.H0 J.H0
   JZ core_sys_write_exit
   LD @H.H0 B.B0
   OUT B $7F
   INC A
   DEC J.H0
   INC H.H0
   JMP core_sys_write_next_char
core_sys_write_exit:
   POP Z.H0
   POP B
   RET


;******************************************************************************
; sys_reboot system call

; These are the "magic numbers" that affect this system call.

; #define LINUX_REBOOT_CMD_RESTART        0x01234567
; #define LINUX_REBOOT_CMD_HALT           0xCDEF0123
; #define LINUX_REBOOT_CMD_CAD_ON         0x89ABCDEF
; #define LINUX_REBOOT_CMD_CAD_OFF        0x00000000
; #define LINUX_REBOOT_CMD_POWER_OFF      0x4321FEDC
; #define LINUX_REBOOT_CMD_RESTART2       0xA1B2C3D4
; #define LINUX_REBOOT_CMD_SW_SUSPEND     0xD000FCE2
; #define LINUX_REBOOT_CMD_KEXEC          0x45584543


LABEL core_sys_reboot_exit AUTO

core_sys_reboot:
   CMP $4321FEDC J.H0         ; This magic number means to power off.
   JNZ core_sys_reboot_exit
   CALL core_os_shut_down
core_sys_reboot_exit:
   RET


;******************************************************************************
; Output the contents of a register G as a hexadecimal number.

LABEL core_os_hex_print_low_nybble  AUTO
LABEL core_os_hex_string            AUTO

core_os_hex_print_w0:
   CALL core_os_hex_print_b7
   CALL core_os_hex_print_b6
   CALL core_os_hex_print_b5
   CALL core_os_hex_print_b4
   CALL core_os_hex_print_b3
   CALL core_os_hex_print_b2
   CALL core_os_hex_print_b1
   CALL core_os_hex_print_b0
   RET

core_os_hex_print_h1:
   CALL core_os_hex_print_b7
   CALL core_os_hex_print_b6
   CALL core_os_hex_print_b5
   CALL core_os_hex_print_b4
   RET

core_os_hex_print_h0:
   CALL core_os_hex_print_b3
   CALL core_os_hex_print_b2
   CALL core_os_hex_print_b1
   CALL core_os_hex_print_b0
   RET

core_os_hex_print_q3:
   CALL core_os_hex_print_b7
   CALL core_os_hex_print_b6
   RET

core_os_hex_print_q2:
   CALL core_os_hex_print_b5
   CALL core_os_hex_print_b4
   RET

core_os_hex_print_q1:
   CALL core_os_hex_print_b3
   CALL core_os_hex_print_b2
   RET

core_os_hex_print_q0:
   CALL core_os_hex_print_b1
   CALL core_os_hex_print_b0
   RET

core_os_hex_print_b7:
   PUSH G.B0
   LD G.B7 G.B0
   CALL core_os_hex_print_b0
   POP G.B0
   RET

core_os_hex_print_b6:
   PUSH G.B0
   LD G.B6 G.B0
   CALL core_os_hex_print_b0
   POP G.B0
   RET

core_os_hex_print_b5:
   PUSH G.B0
   LD G.B5 G.B0
   CALL core_os_hex_print_b0
   POP G.B0
   RET

core_os_hex_print_b4:
   PUSH G.B0
   LD G.B4 G.B0
   CALL core_os_hex_print_b0
   POP G.B0
   RET

core_os_hex_print_b3:
   PUSH G.B0
   LD G.B3 G.B0
   CALL core_os_hex_print_b0
   POP G.B0
   RET

core_os_hex_print_b2:
   PUSH G.B0
   LD G.B2 G.B0
   CALL core_os_hex_print_b0
   POP G.B0
   RET

core_os_hex_print_b1:
   PUSH G.B0
   LD G.B1 G.B0
   CALL core_os_hex_print_b0
   POP G.B0
   RET

; Convert byte
core_os_hex_print_b0:
   PUSH G.B0
   SHR $04 G.B0
   CALL core_os_hex_print_low_nybble
   POP G.B0
   AND $0F G.B0
   CALL core_os_hex_print_low_nybble
   RET

core_os_hex_print_low_nybble:
   PUSH G
   CLR A
   LD G.B0 A.B0
   AND $0F A.B0
   LD core_os_hex_string G.H0      ; Load the pointer to the string into H.H0 register
   ADD A.B0 G.H0
   LD @G.H0 A.B0
   LD $0A A.B1
   OUT A $7F
   POP G
   RET


;******************************************************************************
; The output functions index into this string to perform the output

core_os_hex_string: 
   STRING "0123456789ABCDEF"


;******************************************************************************
; Output a new line to the console.

LABEL core_os_label_newline            AUTO

core_os_print_newline:
   PUSH A
   PUSH G
   LD core_os_label_newline G
   CALL core_os_puts
   POP G
   POP A
   RET


;******************************************************************************
;

core_os_print_reg:
   PUSH G
   PUSH H
   CALL core_os_puts
   LD H G
   CALL core_os_hex_print_w0
   CALL core_os_print_newline
   POP H
   POP G
   RET


;******************************************************************************
; 

LABEL core_os_exception_label          AUTO
LABEL core_os_exception_segment_label  AUTO
LABEL core_os_exception_address_label  AUTO
LABEL core_os_label_reg_a              AUTO
LABEL core_os_label_reg_b              AUTO
LABEL core_os_label_reg_c              AUTO
LABEL core_os_label_reg_d              AUTO
LABEL core_os_label_reg_e              AUTO
LABEL core_os_label_reg_g              AUTO
LABEL core_os_label_reg_h              AUTO
LABEL core_os_label_reg_j              AUTO
LABEL core_os_label_reg_k              AUTO
LABEL core_os_label_reg_l              AUTO
LABEL core_os_label_reg_m              AUTO
LABEL core_os_label_reg_z              AUTO
LABEL core_os_label_reg_f              AUTO
LABEL core_os_label_reg_i              AUTO
LABEL core_os_label_reg_p              AUTO
LABEL core_os_label_reg_s              AUTO

core_os_exception_handler:
   ; Maybe I need a PUSHALL instruction...
   PUSH Z
   PUSH M
   PUSH L
   PUSH K
   PUSH J
   PUSH H
   PUSH G
   PUSH E
   PUSH D
   PUSH C
   PUSH B
   PUSH A

   LD core_os_label_reg_a G
   POP H
   CALL core_os_print_reg

   LD core_os_label_reg_b G
   POP H
   CALL core_os_print_reg

   LD core_os_label_reg_c G
   POP H
   CALL core_os_print_reg

   LD core_os_label_reg_d G
   POP H
   CALL core_os_print_reg

   LD core_os_label_reg_e G
   POP H
   CALL core_os_print_reg

   LD core_os_label_reg_g G
   POP H
   CALL core_os_print_reg

   LD core_os_label_reg_h G
   POP H
   CALL core_os_print_reg

   LD core_os_label_reg_j G
   POP H
   CALL core_os_print_reg

   LD core_os_label_reg_k G
   POP H
   CALL core_os_print_reg

   LD core_os_label_reg_l G
   POP H
   CALL core_os_print_reg

   LD core_os_label_reg_m G
   POP H
   CALL core_os_print_reg

   LD core_os_label_reg_z G
   POP H
   CALL core_os_print_reg

   ; Z.B0 = exception number
   POP Z.B0

   ; M.H0 = stack frame
   LD S.H0 M.H0
   LD core_os_label_reg_p G
   LD @M.H0 H
   LD H B
   CALL core_os_print_reg

   ADD $08 M.H0
   LD core_os_label_reg_f G
   LD @M.H0 H
   CALL core_os_print_reg

   ADD $08 M.H0
   LD core_os_label_reg_i G
   LD @M.H0 H
   CALL core_os_print_reg

   ADD $08 M.H0
   LD core_os_label_reg_s G
   LD @M.H0 H
   CALL core_os_print_reg

   LD core_os_exception_label G
   CALL core_os_puts
   LD Z.B0 G.B0
   CALL core_os_hex_print_b0
   LD core_os_exception_segment_label G
   CALL core_os_puts
   LD B G
   CALL core_os_hex_print_h1
   LD core_os_exception_address_label G
   CALL core_os_puts
   LD B G
   CALL core_os_hex_print_h0
   CALL core_os_shut_down
   IRET

core_os_exception_handler_00:
   PUSH $00
   JMP core_os_exception_handler

core_os_exception_handler_01:
   PUSH $01
   JMP core_os_exception_handler

core_os_exception_handler_02:
   PUSH $02
   JMP core_os_exception_handler

core_os_exception_handler_03:
   PUSH $03
   JMP core_os_exception_handler

core_os_exception_handler_04:
   PUSH $04
   JMP core_os_exception_handler

core_os_exception_handler_05:
   PUSH $05
   JMP core_os_exception_handler

core_os_exception_handler_06:
   PUSH $06
   JMP core_os_exception_handler

core_os_exception_handler_07:
   PUSH $07
   JMP core_os_exception_handler

core_os_exception_handler_08:
   PUSH $08
   JMP core_os_exception_handler

core_os_exception_handler_09:
   PUSH $09
   JMP core_os_exception_handler

core_os_exception_handler_0A:
   PUSH $0A
   JMP core_os_exception_handler

core_os_exception_handler_0B:
   PUSH $0B
   JMP core_os_exception_handler

core_os_exception_handler_0C:
   PUSH $0C
   JMP core_os_exception_handler

core_os_exception_handler_0D:
   PUSH $0D
   JMP core_os_exception_handler

core_os_exception_handler_0E:
   PUSH $0E
   JMP core_os_exception_handler

core_os_exception_handler_0F:
   PUSH $0F
   JMP core_os_exception_handler

core_os_label_reg_a:
   STRING "A: $\0"

core_os_label_reg_b:
   STRING "B: $\0"

core_os_label_reg_c:
   STRING "C: $\0"

core_os_label_reg_d:
   STRING "D: $\0"

core_os_label_reg_e:
   STRING "E: $\0"

core_os_label_reg_g:
   STRING "G: $\0"

core_os_label_reg_h:
   STRING "H: $\0"

core_os_label_reg_j:
   STRING "J: $\0"

core_os_label_reg_k:
   STRING "K: $\0"

core_os_label_reg_l:
   STRING "L: $\0"

core_os_label_reg_m:
   STRING "M: $\0"

core_os_label_reg_z:
   STRING "Z: $\0"

core_os_label_reg_f:
   STRING "F: $\0"

core_os_label_reg_i:
   STRING "I: $\0"

core_os_label_reg_p:
   STRING "P: $\0"

core_os_label_reg_s:
   STRING "S: $\0"

core_os_label_newline:
   STRING "\n\0"

core_os_exception_label:
   STRING "\nException \0"

core_os_exception_segment_label:
   STRING " at segment $\0"

core_os_exception_address_label:
   STRING " address $\0"
