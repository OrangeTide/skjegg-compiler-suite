REM consume the MathUtil module: import a FN and a CONST with FROM .. IMPORT,
REM call the imported FN across the archive boundary, and fold the imported CONST
FROM MathUtil IMPORT Hypot, MaxSides

DIM h AS FLOAT
h = Hypot(3.0, 4.0)
PRINT "hypot="; h
PRINT "sides="; MaxSides
PRINT "scaled="; Hypot(6.0, 8.0)
END
