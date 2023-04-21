; **********************************************************************************
; The entry point. Execution begins at segment $00000000, address $00000000

$0000`0000:             ; The back-tick (`)  is used as a number separator. 
                        ; Underscore (_) and comma (,) may also be used as separators.
    CALL main
    HALT                ; For now, HALT exits the Maize interpreter, but in the 
                        ; future it will pause the CPU and wait for an interrupt.

; **********************************************************************************
; The output message

hw_string:
    STRING "Hello, world!\0"

; **********************************************************************************
; Return the length of a zero-terminated string. Equivalent to the following C code:
;
;   int strlen(char const *str) {
;       int len = 0;
;       while (str[len]) {
;           ++len;
;       }
;       return len;
;   }
;
; Parameters:
;   R0.H0: Address of string
; Return: 
;   RV: Length of string

strlen:
    PUSH BP                 ; Save the base pointer
    LD SP BP                ; Copy the stack pointer to the base pointer
    SUB $04 SP              ; Make room for a 32-bit (4-byte) counter on the stack
    LEA $-04 BP RT.H0       ; Load the address of the counter's stack location into RT.H0
    ST $00 @RT.H0           ; Set the counter to zero.
loop_condition:
	LEA @RT.H0 R0.H0 R1.H0  ; Add the counter value to the address and put the result into H.H0.
    LD @R1.H0 R1.B4         ; Copy the character at the address in R1.H0 into R1.B4.
	JZ loop_exit            ; LD sets the zero flag if the value copied to R1.B4 is zero.
loop_body:
    LD @RT.H0 RT.H1         ; Put the counter value at RT.H0 into RT.H1
	INC RT.H1               ; Add 1 to the temporary
    ST RT.H1 @RT.H0         ; Store the new value back into the counter's address
	JMP loop_condition      ; ...and continue the loop.
loop_exit:
	LD @RT.H0 RV            ; Put the (sign-extended) counter value into RV, the return register.
    LD BP SP                ; Restore the stack
    POP BP                  ; Restore the base pointer
	RET                     ; Pop return address from stack and place into PC.

; **********************************************************************************
; The main function

main:
    LD hw_string R0.H0      ; Put address of message string into R0.H0
    CALL strlen             ; Call strlen function to get the string length
    LD $01 R0               ; $01 in R0 indicates output to stdout
    LD hw_string R1.H0      ; R1.H0 holds address of message to output
    LD RV R2                ; Put the string length into R2
    SYS $01                 ; Call output function implemented in Maize VM
    LD $0 RV                ; Set return value for main
    RET                     ; Leave main
