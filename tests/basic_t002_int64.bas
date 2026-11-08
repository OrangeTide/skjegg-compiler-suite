REM 64-bit INT range and arithmetic (doc/languages/basic.md: INT is 64-bit)
DIM big AS INT
big = 1000000 * 1000000
PRINT "big="; big
PRINT "sum="; 2147483647 + 1
PRINT "neg="; 0 - 9223372036854775807
PRINT 1000000000000 DIV 7, 1000000000000 MOD 7
END
