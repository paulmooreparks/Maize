INCLUDE "core.asm"
INCLUDE "stdlib.asm"

LABEL foobar AUTO

$00001000:
   ; Let's put some values in the general-purpose registers so that we can see something 
   ; in the register dump later.

   LD $01 A
   LD $02 B
   LD $03 C
   LD $04 D
   LD $05 E
   LD $06 G
   LD $07 H
   LD $08 J
   LD $09 K
   LD $0A L
   LD $0B M
   LD $0C Z

   ; This will trigger a divide-by-zero exception, which is exception $00 (same as x86).
   ; You will see a register dump and an exception report on the console screen.

   DIV $00 A

   ; Execution will not get this far because of the exception above... unless you roll your own 
   ; exception handler and put its address at segment $0000`0000 address $0000`0000. 

   HALT

