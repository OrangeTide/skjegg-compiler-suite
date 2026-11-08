REM procedures and functions
DECLARE FN fact(n AS INT) AS INT

DIM r AS INT
DIM f AS FLOAT

PROC greet(n AS INT)
  PRINT "hello "; n
ENDPROC

FN add(a AS INT, b AS INT) AS INT
  RETURN a + b
ENDFN

FN maxof(a AS INT, b AS INT) AS INT
  IF a > b THEN RETURN a
  RETURN b
ENDFN

FN half(x AS FLOAT) AS FLOAT
  RETURN x / 2
ENDFN

FN fact(n AS INT) AS INT
  IF n <= 1 THEN RETURN 1
  RETURN n * fact(n - 1)
ENDFN

PROC countdown(n AS INT)
  WHILE n > 0
    PRINT "t="; n
    n = n - 1
  ENDWHILE
ENDPROC

greet(42)
r = add(3, 4)
PRINT "add="; r
PRINT "max="; maxof(10, 7)
PRINT "fact5="; fact(5)
f = half(7.0)
PRINT "half="; f
PRINT "nested="; add(add(1, 2), add(3, 4))
countdown(3)
REM FN call in statement position (result discarded)
add(99, 1)
PRINT "done"
END
