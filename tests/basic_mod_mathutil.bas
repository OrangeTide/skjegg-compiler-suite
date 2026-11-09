REM a MODULE exercising EXPORT CONST / FN / PROC, REF params, and a private FN
MODULE MathUtil

EXPORT CONST MaxSides = 32, Tau = 6

EXPORT FN Hypot(a AS FLOAT, b AS FLOAT) AS FLOAT
  RETURN SQR(a * a + b * b)
ENDFN

EXPORT PROC Bump(REF n AS INT, by AS INT)
  n = n + by
ENDPROC

FN Secret(x AS INT) AS INT
  RETURN x * 2
ENDFN
