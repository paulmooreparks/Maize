LABEL stdlib_strlen AUTO
LABEL stdlib_strupr AUTO
LABEL stdlib_putchar AUTO
LABEL stdlib_getchar AUTO
LABEL stdlib_puts AUTO
LABEL stdlib_shutdown AUTO


;******************************************************************************
; strlen

LABEL stdlib_strlen_loop AUTO
LABEL stdlib_strlen_done AUTO

stdlib_strlen:
   PUSH G.H0
   CLR A
stdlib_strlen_loop:
   CMPIND $00 @G.H0
   JZ stdlib_strlen_done
   INC A.H0
   INC G.H0
   JMP stdlib_strlen_loop
stdlib_strlen_done:
   POP G.H0
   RET


;******************************************************************************
; strupr

LABEL stdlib_strupr_loop AUTO
LABEL stdlib_strupr_done AUTO
LABEL stdlib_strupr_continue AUTO

stdlib_strupr:
   PUSH G.H0
   CLR A
stdlib_strupr_loop:
   CMPIND $00 @G.H0
   JZ stdlib_strupr_done
   CMPIND $61 @G.H0
   JB stdlib_strupr_continue
   CMPIND $7A @G.H0
   JA stdlib_strupr_continue
   LD @G.H0 A.B0
   SUB $20 A.B0
   ST A.B0 @G.H0
stdlib_strupr_continue:
   INC G.H0
   JMP stdlib_strupr_loop
stdlib_strupr_done:
   POP G.H0
   LD G.H0 A
   RET


;******************************************************************************
; strlwr

LABEL stdlib_strlwr_loop AUTO
LABEL stdlib_strlwr_done AUTO
LABEL stdlib_strlwr_continue AUTO

stdlib_strlwr:
   PUSH G.H0
   CLR A
stdlib_strlwr_loop:
   CMPIND $00 @G.H0
   JZ stdlib_strlwr_done
   CMPIND $41 @G.H0
   JB stdlib_strlwr_continue
   CMPIND $5A @G.H0
   JA stdlib_strlwr_continue
   LD @G.H0 A.B0
   ADD $20 A.B0
   ST A.B0 @G.H0
stdlib_strlwr_continue:
   INC G.H0
   JMP stdlib_strlwr_loop
stdlib_strlwr_done:
   POP G.H0
   LD G.H0 A
   RET


;******************************************************************************
; puts

stdlib_puts:
   PUSH A
   LD $0002 A
   INT $40
   POP A
   RET


;******************************************************************************
; putchar

stdlib_putchar:
   CLR A
   LD $0003 A
   INT $40
   CLR A
   LD G.B0 A.B0
   RET


;******************************************************************************
; getchar

stdlib_getchar:
   RET


;******************************************************************************
; shutdown

stdlib_shutdown:
   LD $0000 A
   INT $40
   RET
