LABEL hw_start          $00001000
LABEL hw_string         AUTO

hw_start:
LD $00001000 S.H0 
LD S.H0 S.H1
PUSH $1234
POP A.Q0
PUSH A.Q0
POP B.B0
LD $01 G
LD hw_string H.H0
LD $0D J
SYS $01
LD $00 G
LD $00002100 H.H0
LD $01 J
SYS $00
LD $01 G
LD $00002100 H.H0
LD $01 J
SYS $01
HALT

hw_string: 
   STRING "Hello, world!\0"
