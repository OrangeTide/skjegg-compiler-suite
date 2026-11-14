REM Geometry: a small BASIC module (a library with no entry point).
REM
REM It exports a constant, an opaque Circle handle with its accessor routines,
REM and a couple of plain functions. The fields of Circle are private to this
REM file: a program that imports the module sees only the handle type and the
REM exported routines, never cx/cy/r directly. The private helper Pi below is
REM not EXPORTed, so it is invisible to importers too.
REM
REM Build it into an archive, then consume it from shapes.bas. See README.md.
MODULE Geometry

EXPORT CONST MaxSides = 32

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

EXPORT FN Radius(c AS Circle) AS FLOAT
  RETURN c.r
ENDFN

EXPORT FN Diameter(c AS Circle) AS FLOAT
  RETURN c.r + c.r
ENDFN

EXPORT FN Area(c AS Circle) AS FLOAT
  RETURN Pi() * c.r * c.r
ENDFN

EXPORT PROC SetRadius(c AS Circle, v AS FLOAT)
  c.r = v
ENDPROC

REM private helper, not exported: the importer cannot see or call it
FN Pi() AS FLOAT
  RETURN 3.14159265
ENDFN
