REM BYTE: 0..255, promotes to INT in arithmetic, truncates on assignment
DIM b AS BYTE
DIM c AS BYTE
b = 200
c = 100
PRINT "b="; b
PRINT "sum="; b + c
PRINT "wrap="; b + c
c = b + c
PRINT "trunc="; c
b = 255 + 1
PRINT "zero="; b
b = -1
PRINT "neg="; b
b = 300
PRINT "mod256="; b
PRINT "cmp="; c > 10
END
