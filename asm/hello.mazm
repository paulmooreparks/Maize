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
;   size_t strlen(char const *str) {
;       size_t len = 0;
;       while (str[len]) {
;           ++len;
;       }
;       return len;
;   }
;
; Maize uses a flat 64-bit address space, so pointers and the stack pointer are full
; 64-bit values; addresses live in whole registers, not H0 sub-registers.
;
; Parameters:
;   R0: Address of string
; Return:
;   RV: Length of string

strlen:
    PUSH BP                 ; Save the caller's base pointer
    CP SP BP                ; Establish this frame: BP = SP
    SUB $08 SP              ; Reserve an 8-byte local slot for the counter
    LEA $-08 BP RT          ; RT = address of the counter (BP - 8), a full 64-bit address
    CLR R2                  ; counter = 0
    ST R2 @RT               ; Store the counter to its stack slot
loop_condition:
    LD @RT R2               ; Load the counter
    LEA R2 R0 R1            ; R1 = string address + counter
    LD @R1 R3.B0            ; R3.B0 = the character at that address
    CMP $00 R3.B0           ; Data movement does not set flags; test the byte explicitly.
    JZ loop_exit            ; Jump out of the loop when the terminating NUL is reached.
loop_body:
    LD @RT R2               ; Load the counter
    INC R2                  ; ...add one...
    ST R2 @RT               ; ...and store it back.
    JMP loop_condition      ; Continue the loop.
loop_exit:
    LD @RT RV               ; Return the counter in RV.
    CP BP SP                ; Tear down the frame: SP = BP
    POP BP                  ; Restore the caller's base pointer
    RET                     ; Pop the return address from the stack into PC.

; **********************************************************************************
; The main function

main:
    CLR R0
    CP hw_string R0.H0      ; Copy the address of the message string into R0
    CALL strlen             ; Call strlen to get the string length (into RV)
    CP $01 R0               ; $01 in R0 indicates output to stdout
    CLR R1
    CP hw_string R1.H0      ; R1 holds the address of the message to output
    CP RV R2                ; Put the string length into R2
    SYS $01                 ; Call the output function implemented in the Maize VM
    CP $00 RV               ; Set the return value for main
    RET                     ; Leave main
