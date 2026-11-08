REM arrays: element types, multi-dim, in loops and expressions
DIM a(5) AS INT
DIM f(3) AS FLOAT
DIM b(4) AS BYTE
DIM s(3) AS STRING
DIM g(3, 4) AS INT
DIM i AS INT
DIM j AS INT
DIM sum AS INT

FOR i = 0 TO 4
  a(i) = i * i
NEXT i
PRINT "a="; a(0); " "; a(1); " "; a(2); " "; a(3); " "; a(4)

sum = 0
FOR i = 0 TO 4
  sum = sum + a(i)
NEXT i
PRINT "sum="; sum

f(0) = 1.5
f(1) = 2.5
f(2) = f(0) + f(1)
PRINT "f="; f(2)

b(0) = 300
b(1) = 65
PRINT "b="; b(0); " "; b(1)

s(0) = "foo"
s(1) = "bar"
PRINT "s="; s(0) + s(1)

FOR i = 0 TO 2
  FOR j = 0 TO 3
    g(i, j) = i * 10 + j
  NEXT j
NEXT i
PRINT "g="; g(0, 0); " "; g(1, 2); " "; g(2, 3)
END
