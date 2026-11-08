REM TYPE records: fields, assignment, REF parameters

TYPE Point
    x AS INT
    y AS INT
    name AS STRING
ENDTYPE

PROC shift(REF p AS Point, dx AS INT, dy AS INT)
    p.x = p.x + dx
    p.y = p.y + dy
ENDPROC

DIM a AS Point
a.x = 3
a.y = 4
a.name = "origin"

PRINT a.x
PRINT a.y
PRINT a.name

REM whole-record assignment
DIM b AS Point
b = a
b.x = 100
PRINT a.x
PRINT b.x
PRINT b.name

REM REF record parameter mutates caller's record
shift(a, 10, 20)
PRINT a.x
PRINT a.y

END
