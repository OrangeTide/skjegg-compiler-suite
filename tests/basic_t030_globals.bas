REM module-scope variables are globals, visible in every routine; a local
REM starts at its zero value (grid(1)/grid(3) and sum are never assigned)
TYPE Counter
    label AS STRING
    n AS INT
ENDTYPE

DIM total AS INT
DIM grid(4) AS BYTE
DIM title AS STRING
DIM c AS Counter
DIM nums AS VECTOR OF INT

PROC init()
    title = "scores"
    c.label = "hits"
    grid(0) = 1
    grid(2) = 7
ENDPROC

PROC add(v AS INT)
    total = total + v
    c.n = c.n + 1
    APPEND nums, v
ENDPROC

FN average() AS INT
    DIM sum AS INT
    DIM i AS INT
    FOR i = 0 TO COUNT(nums) - 1
        sum = sum + nums(i)
    NEXT i
    RETURN sum DIV COUNT(nums)
ENDFN

init()
add(10)
add(20)
add(30)
PRINT title; ": total="; total; " count="; c.n; " avg="; average()
PRINT "grid="; grid(0); grid(1); grid(2); grid(3)
PRINT "label="; c.label
END
