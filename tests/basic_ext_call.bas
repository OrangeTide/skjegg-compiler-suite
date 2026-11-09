REM EXTERNAL end to end: declare three C `int` shims, call them, and print the
REM results. This exercises the C-int boundary both ways. ExtPick's four
REM arguments catch a missing argument narrow (a 64-bit INT passed where a 32-bit
REM int is expected would land in the wrong register/slot and shift the rest),
REM and ExtNeg catches a missing result widen (a negative C int must sign-extend
REM back to a full INT). See doc/languages/basic-modules.md (EXTERNAL).
DECLARE FN ExtAdd(a AS INT, b AS INT) AS INT EXTERNAL "ext_add"
DECLARE FN ExtNeg(x AS INT) AS INT EXTERNAL "ext_neg"
DECLARE FN ExtPick(a AS INT, b AS INT, c AS INT, d AS INT) AS INT EXTERNAL "ext_pick"

PRINT ExtAdd(40, 2)
PRINT ExtNeg(7)
PRINT ExtPick(1, 2, 3, 4)
END
