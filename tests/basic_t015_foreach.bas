REM FOR EACH over arrays, including reference-semantics write-back

DIM a(5) AS INT
DIM i AS INT
FOR i = 0 TO 4
    a(i) = (i + 1) * 10
NEXT i

REM read-only walk: sum the elements
DIM sum AS INT
sum = 0
FOR EACH e IN a
    sum = sum + e
NEXT e
PRINT sum

REM reference semantics: doubling e mutates the array
FOR EACH e IN a
    e = e * 2
NEXT
PRINT a(0)
PRINT a(4)

REM a STRING array
DIM names(3) AS STRING
names(0) = "al"
names(1) = "bo"
names(2) = "cy"
FOR EACH nm IN names
    PRINT nm
NEXT

END
