REM integer operators: comparisons, logical (short-circuit), bitwise, power
PRINT "gt="; 6 > 4
PRINT "eq="; 6 = 4
PRINT "ne="; 6 <> 4
PRINT "and="; 1 AND 0
PRINT "or="; 0 OR 5
PRINT "not0="; NOT 0
PRINT "not5="; NOT 5
PRINT "band="; 12 BAND 10
PRINT "bor="; 12 BOR 10
PRINT "bxor="; 12 BXOR 10
PRINT "shl="; 1 SHL 4
PRINT "shr="; 256 SHR 2
PRINT "bnot="; BNOT 0
PRINT "pow="; 2 ^ 10
PRINT "prec="; 2 + 3 * 4
PRINT "cmpprec="; 2 + 3 > 4
PRINT "sc="; 0 AND (1 DIV 0)
PRINT "sc2="; 1 OR (1 DIV 0)
END
