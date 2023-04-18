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
;   A.H0: Address of string
; Return: 
;   A: Length of string

strlen:
    PUSH BP             ; Save the base pointer
    LD SP BP            ; Copy the stack pointer to the base pointer
    SUB $04 SP          ; Make room for a 32-bit (4-byte) counter on the stack
    LEA $-04 BP Z.H0    ; Load the address of the counter's stack location into Z.H0
    ST $00 @Z.H0        ; Set the counter to zero.
loop_condition:
	LEA @Z.H0 A.H0 H.H0 ; Add the counter value to the address and put the result into H.H0.
    LD @H.H0 H.B4       ; Copy the character at the address in H.H0 into H.B4.
	JZ loop_exit        ; LD sets the zero flag if the value copied to H.B4 is zero.
loop_body:
    LD @Z.H0 Z.H1       ; Put the counter value at Z.H0 into a temporary register, Z.H1
	INC Z.H1            ; Add 1 to the temporary
    ST Z.H1 @Z.H0       ; Store the new value back into the counter's address
	JMP loop_condition  ; ...and continue the loop.
loop_exit:
	LD @Z.H0 A          ; Put the (sign-extended) counter value into A, the return register.
    LD BP SP            ; Restore the stack
    POP BP              ; Restore the base pointer
	RET                 ; Pop return address from stack and place into PC.

; **********************************************************************************
; The main function

main:
    LD hw_string A.H0   ; Put address of message string into A.H0
    CALL strlen         ; Call strlen function to get the string length
    LD $01 G            ; $01 in G indicates output to stdout
    LD hw_string H.H0   ; H.H0 holds address of message to output
    LD A J              ; Put the string length into J
    SYS $01             ; Call output function implemented in Maize VM
    LD $0 A             ; Set return value for main
    RET                 ; Leave main
