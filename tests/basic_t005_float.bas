REM FLOAT literals, variables, arithmetic, promotion, comparisons, PI
DIM r AS FLOAT
DIM n AS INT
n = 7
r = 3.5
PRINT "r="; r
PRINT "half="; 1 / 2
PRINT "pi="; PI
PRINT "mix="; r + n
PRINT "div="; 7 / 2
PRINT "cmp="; 3.5 > 3.0; 2.0 = 2.0; 1.5 <> 1.5
PRINT "mixcmp="; n > 6.5; r < n
PRINT "neg="; -r
PRINT "two="; 2.0
PRINT "negfrac="; 0.0 - 1.25
r = n
PRINT "fromint="; r
r = r / 2
PRINT "acc="; r
PRINT "big="; 1000000.5
END
