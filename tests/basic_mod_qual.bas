REM qualified import: IMPORT M brings the module's exports into scope under
REM dotted names, M.name, resolved against the same archive FROM .. IMPORT uses
IMPORT MathUtil

PRINT "hypot="; MathUtil.Hypot(3.0, 4.0)
PRINT "sides="; MathUtil.MaxSides
PRINT "tau="; MathUtil.Tau
PRINT "diag="; MathUtil.Hypot(5.0, 12.0)
END
