REM an EXTERNAL module: each DECLARE binds a BASIC name to a foreign C symbol
REM (a routine in a linked library) with no BASIC body. Its primitive INT
REM parameters and result cross at the C `int` width, so the .bmi records them as
REM i32, not the 64-bit i64 a BASIC-to-BASIC routine uses. This is how the libpsx
REM binding (units/psx.bas) reaches the PlayStation SDK.
MODULE extdemo
EXPORT DECLARE PROC Blank(r AS INT, g AS INT, b AS INT) EXTERNAL "demo_blank"
EXPORT DECLARE PROC Init() EXTERNAL "demo_init"
EXPORT DECLARE FN Add(a AS INT, b AS INT) AS INT EXTERNAL "demo_add"
