; *****************************************************************************
; The entry point. Execution begins at segment $00000000, address $00000000

$0000`0000:             ; The back-tick (`)  is used as a number separator. 
                        ; Underscore (_) and comma (,) may also be used as separators.
    CALL main
    HALT                ; For now, HALT exits the Maize interpreter, but in the 
                        ; future it will pause the CPU and wait for an interrupt.

; *****************************************************************************
; The output message

hw_string:
    STRING "Hello, world!\0"

; *****************************************************************************
; Return the length of a zero-terminated string. 
; A.H0: Address of string

strlen:
    LD $0 G.H0          ; Set counter to zero.
loop:
	LEA G.H0 A.H0 H.H0  ; Add the counter to the address and put the result into H.H0.
    LD @H.H0 H.B4       ; Dereference H.H0 and copy character at that address into H.B4.
	JZ loop_exit        ; LD sets the zero flag if the value copied to H.B4 is zero.
	INC G.H0            ; Add 1 to the counter...
	JMP loop            ; ...and continue the loop.
loop_exit:
	LD G.H0 A           ; Put the counter value into A, which is the return register.
	RET                 ; Pop return address from stack and place into PC.

; *****************************************************************************
; The main function

main:
    LD hw_string A.H0   ; Put address of message string into A.H0
    CALL strlen         ; Call strlen function to get the string length
    LD $01 G            ; $01 in G indicates output to stdout
    LD hw_string H.H0   ; H.H0 holds address of message to output
    LD A.H0 J           ; Put the string length into J
    SYS $01             ; Call output function implemented in Maize VM
    LD $0 A             ; Set return value for main
    RET                 ; Leave main
