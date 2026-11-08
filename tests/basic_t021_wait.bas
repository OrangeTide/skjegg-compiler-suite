REM WAIT pauses for the given milliseconds; TIME measures elapsed seconds

DIM t0 AS FLOAT
DIM t1 AS FLOAT
t0 = TIME
PRINT "start"
WAIT 50
t1 = TIME
PRINT "end"
IF t1 - t0 >= 0.02 THEN
    PRINT "elapsed"
ENDIF
END
