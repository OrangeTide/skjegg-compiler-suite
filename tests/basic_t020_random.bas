DIM junk AS INT
junk = SEED(12345)
DIM a AS FLOAT
DIM b AS FLOAT
a = RND
b = RND
IF a >= 0.0 AND a < 1.0 THEN
    PRINT "a in range"
ENDIF
IF b >= 0.0 AND b < 1.0 THEN
    PRINT "b in range"
ENDIF
REM reseed and confirm the sequence repeats
junk = SEED(12345)
DIM c AS FLOAT
c = RND
IF c = a THEN
    PRINT "repeatable"
ENDIF
DIM t AS FLOAT
t = TIME
IF t >= 0.0 THEN
    PRINT "time ok"
ENDIF
END
