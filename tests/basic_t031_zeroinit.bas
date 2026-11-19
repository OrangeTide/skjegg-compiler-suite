REM a declared local starts at its zero value even after the stack was dirtied
PROC dirty()
    DIM x AS INT
    DIM y AS INT
    DIM z AS STRING
    x = 123456
    y = 7777
    z = "poison"
    PRINT "dirty "; x; " "; y; " "; z
ENDPROC

FN probe() AS INT
    DIM a AS INT
    DIM f AS FLOAT
    DIM s AS STRING
    DIM arr(3) AS INT
    PRINT "a="; a; " f="; f; " s=["; s; "] arr="; arr(0); ","; arr(2)
    RETURN a
ENDFN

DIM r AS INT
dirty()
r = probe()
PRINT "probe returned "; r
END
