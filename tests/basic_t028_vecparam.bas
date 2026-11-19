REM vectors as parameters (by reference) and FN returns
TYPE P
  x AS INT
  y AS INT
ENDTYPE

PROC fill(v AS VECTOR OF INT, lo AS INT, hi AS INT)
  DIM i AS INT
  FOR i = lo TO hi
    APPEND v, i * i
  NEXT i
ENDPROC

FN total(v AS VECTOR OF INT) AS INT
  DIM s AS INT
  s = 0
  FOR EACH e IN v
    s = s + e
  NEXT
  RETURN s
ENDFN

FN makeRange(lo AS INT, hi AS INT) AS VECTOR OF INT
  DIM r AS VECTOR OF INT
  DIM i AS INT
  FOR i = lo TO hi
    APPEND r, i
  NEXT i
  RETURN r
ENDFN

REM pass a vector by reference: the callee's APPENDs are visible here
DIM v AS VECTOR OF INT
fill(v, 1, 4)
PRINT "count="; COUNT(v)
PRINT "sum="; total(v)

REM receive a vector from an FN
DIM r AS VECTOR OF INT
r = makeRange(10, 14)
PRINT "r="; COUNT(r); " "; r(0); " "; r(4); " "; total(r)

REM a record-vector parameter, mutated in place
PROC bump(ps AS VECTOR OF P)
  FOR EACH e IN ps
    e.x = e.x + 100
  NEXT
ENDPROC
DIM ps AS VECTOR OF P
DIM p AS P
p.x = 1
p.y = 2
APPEND ps, p
p.x = 3
APPEND ps, p
bump(ps)
PRINT "ps="; ps(0).x; " "; ps(1).x
END
