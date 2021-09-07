INCLUDE "core.asm"
INCLUDE "stdlib.asm"

LABEL _start            $00001000
LABEL output            AUTO
LABEL first_word        AUTO
LABEL second_word       AUTO
LABEL message           AUTO
LABEL prompt            AUTO
LABEL color_1           AUTO
LABEL color_2           AUTO

; Startup routine
_start:

   ; Set background color
   CLR A
   LD $0B A.B1
   LD $00 A.B3
   LD $00 A.B2 ; Black
   INT $10

   ; Clear screen
   CLR A
   LD $04 A.B1
   INT $10

   ; Set foreground color
   CLR A
   LD $0B A.B1
   LD $01 A.B3
   LD @color_1 A.B2
   INT $10

   ; Write first word
   LD first_word G
   CALL stdlib_puts

   ; Change foreground color
   CLR A
   LD $0B A.B1
   LD $01 A.B3
   LD @color_2 A.B2 
   INT $10

   ; Write second word
   LD second_word G
   CALL stdlib_puts

   ; Set foreground color
   CLR A
   LD $0B A.B1
   LD $01 A.B3
   LD $07 A.B2 ; Gray
   INT $10

   ; Move cursor
   CLR A
   LD $02 A.B1   
   LD $00 A.B6   
   LD $02 A.B7   
   INT $10

   ; Write message
   LD message G
   CALL stdlib_puts

   ; Move cursor
   CLR A
   LD $02 A.B1   
   LD $00 A.B6   
   LD $04 A.B7   
   INT $10

   ; Write prompt
   LD prompt G 
   CALL stdlib_puts

   ; We're done doing setup, so go halt the CPU and wait for interrupts
   LD $01 A
   ; A.Q0 = $00 -> yield idle time to OS
   INT $40

color_1:
   DATA $0E ; Yellow

color_2:
   DATA $0B ; Cyan

first_word: 
   STRING "Maize \0"

second_word:
   STRING "CLI\0"

message:
   STRING "Okay, it's not a CLI yet, but it's the framework for one. Type characters to see echo, or CTRL-C to exit.\0"

prompt:
   STRING "> \0"
