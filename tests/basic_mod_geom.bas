REM a MODULE with an opaque exported TYPE and its accessor surface: a
REM constructor that heap-allocates and returns a handle, a getter, a setter,
REM and a derived FN, all taking the type as an opaque handle (no REF written)
MODULE Geom

EXPORT TYPE Circle
  cx AS FLOAT
  cy AS FLOAT
  r  AS FLOAT
ENDTYPE

EXPORT FN NewCircle(x AS FLOAT, y AS FLOAT, rad AS FLOAT) AS Circle
  DIM c AS Circle
  c.cx = x
  c.cy = y
  c.r = rad
  RETURN c
ENDFN

EXPORT FN CircleR(c AS Circle) AS FLOAT
  RETURN c.r
ENDFN

EXPORT FN CircleDiameter(c AS Circle) AS FLOAT
  RETURN c.r + c.r
ENDFN

EXPORT PROC SetCircleR(c AS Circle, v AS FLOAT)
  c.r = v
ENDPROC
