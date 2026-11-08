REM control flow: IF, WHILE, REPEAT, FOR, EXIT, CASE
DIM i AS INT
DIM n AS INT
DIM sum AS INT

REM single-line IF with ELSE
IF 3 > 2 THEN PRINT "yes" ELSE PRINT "no"
IF 1 > 2 THEN PRINT "bad" ELSE PRINT "else-ok"

REM block IF with ELSEIF / ELSE
n = 2
IF n = 1 THEN
  PRINT "one"
ELSEIF n = 2 THEN
  PRINT "two"
ELSEIF n = 3 THEN
  PRINT "three"
ELSE
  PRINT "many"
ENDIF

REM WHILE with EXIT
i = 0
sum = 0
WHILE i < 100
  i = i + 1
  IF i = 5 THEN EXIT WHILE
  sum = sum + i
ENDWHILE
PRINT "whilesum="; sum

REM REPEAT UNTIL (runs at least once)
i = 0
REPEAT
  i = i + 1
UNTIL i >= 3
PRINT "repeat="; i

REM FOR ascending
sum = 0
FOR i = 1 TO 5
  sum = sum + i
NEXT i
PRINT "for="; sum

REM FOR with STEP, descending
sum = 0
FOR i = 10 TO 2 STEP -2
  sum = sum + i
NEXT
PRINT "fordown="; sum

REM FOR with EXIT FOR
sum = 0
FOR i = 1 TO 1000
  IF i > 4 THEN EXIT FOR
  sum = sum + i
NEXT i
PRINT "forexit="; sum

REM CASE: value, range, relop, else
FOR i = 0 TO 6
  CASE i
  WHEN 0
    PRINT "zero"
  WHEN 1, 2
    PRINT "low"
  WHEN 3 TO 4
    PRINT "mid"
  WHEN > 5
    PRINT "high"
  ELSE
    PRINT "other"
  ENDCASE
NEXT i
END
