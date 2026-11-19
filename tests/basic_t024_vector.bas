REM growable vectors: DIM, APPEND, index read/write, COUNT, growth
DIM v AS VECTOR OF INT
DIM i AS INT
FOR i = 0 TO 4
  APPEND v, i * i
NEXT i
PRINT "count="; COUNT(v)
PRINT "v="; v(0); " "; v(1); " "; v(2); " "; v(3); " "; v(4)

REM overwrite an element in place
v(2) = 99
PRINT "v2="; v(2)

REM sum through indices, bounded by COUNT
DIM sum AS INT
sum = 0
FOR i = 0 TO COUNT(v) - 1
  sum = sum + v(i)
NEXT i
PRINT "sum="; sum

REM growth well past the initial capacity
DIM big AS VECTOR OF INT
FOR i = 0 TO 19
  APPEND big, i
NEXT i
PRINT "big="; COUNT(big); " "; big(0); " "; big(19)

REM a STRING vector
DIM names AS VECTOR OF STRING
APPEND names, "al"
APPEND names, "bo"
APPEND names, "cy"
PRINT "names="; names(0) + names(1) + names(2); " "; COUNT(names)

REM a FLOAT vector
DIM fs AS VECTOR OF FLOAT
APPEND fs, 1.5
APPEND fs, 2.25
PRINT "fs="; fs(0) + fs(1)

REM a BYTE vector (truncates on store)
DIM bs AS VECTOR OF BYTE
APPEND bs, 300
APPEND bs, 65
PRINT "bs="; bs(0); " "; bs(1)
END
