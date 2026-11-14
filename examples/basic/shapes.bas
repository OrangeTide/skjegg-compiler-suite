REM shapes: a program that consumes the Geometry module.
REM
REM FROM Geometry IMPORT pulls the named entities in unqualified, so Circle and
REM the routines read as if they were local. Circle is opaque here: the only way
REM to make one is NewCircle, and the only way to read or change it is through
REM the exported routines. The constant MaxSides comes across the same way.
REM
REM The alternative is `IMPORT Geometry` (no name list), which binds the same
REM entities under dotted names: Geometry.NewCircle, Geometry.Circle,
REM Geometry.MaxSides. See README.md.
FROM Geometry IMPORT Circle, NewCircle, Radius, Diameter, Area, SetRadius, MaxSides

PRINT "MaxSides="; MaxSides

DIM c AS Circle
c = NewCircle(1.0, 2.0, 5.0)
PRINT "radius="; Radius(c)
PRINT "diameter="; Diameter(c)
PRINT "area="; Area(c)

SetRadius(c, 10.0)
PRINT "after SetRadius:"
PRINT "radius="; Radius(c)
PRINT "diameter="; Diameter(c)
END
