REM a declared-but-unassigned STRING reads as empty, everywhere it can live
TYPE Person
    name AS STRING
    age AS INT
ENDTYPE

DIM s AS STRING
PRINT "s=["; s; "] len="; LEN(s)
PRINT "cat=["; s + "tail"; "]"

DIM sa(3) AS STRING
PRINT "sa1=["; sa(1); "]"
sa(1) = "set"
PRINT "sa1=["; sa(1); "]"

DIM p AS Person
PRINT "name=["; p.name; "] age="; p.age

DIM v AS VECTOR OF STRING
APPEND v, "a"
APPEND v, s
PRINT "v0=["; v(0); "] v1=["; v(1); "]"
END
