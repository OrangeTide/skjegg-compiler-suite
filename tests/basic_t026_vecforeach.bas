REM FOR EACH over a vector, read and reference-write
DIM v AS VECTOR OF INT
DIM i AS INT
FOR i = 1 TO 5
  APPEND v, i * 10
NEXT i

REM read-only walk: sum
DIM sum AS INT
sum = 0
FOR EACH e IN v
  sum = sum + e
NEXT e
PRINT "sum="; sum

REM reference write-back: double each element
FOR EACH e IN v
  e = e * 2
NEXT
PRINT "v0="; v(0); " v4="; v(4)

REM walk a STRING vector
DIM names AS VECTOR OF STRING
APPEND names, "al"
APPEND names, "bo"
APPEND names, "cy"
FOR EACH nm IN names
  PRINT nm
NEXT

REM empty vector: the body never runs
DIM empty AS VECTOR OF INT
DIM ran AS INT
ran = 0
FOR EACH x IN empty
  ran = ran + 1
NEXT
PRINT "ran="; ran
END
