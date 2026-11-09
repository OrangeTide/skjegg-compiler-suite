REM qualified import of an opaque TYPE: IMPORT Geom brings the handle type and
REM its accessors in under dotted names. A handle variable is declared as
REM Geom.Circle, constructed and used only through the dotted routines; the
REM opaque identity is carried on the dotted key, so a Geom.NewCircle result and
REM a DIM .. AS Geom.Circle share one type (a mismatch would fail to compile)
IMPORT Geom

DIM c AS Geom.Circle
c = Geom.NewCircle(1.0, 2.0, 5.0)
PRINT "r="; Geom.CircleR(c)
PRINT "d="; Geom.CircleDiameter(c)

DIM c2 AS Geom.Circle
c2 = Geom.NewCircle(0.0, 0.0, 100.0)
Geom.SetCircleR(c, 12.5)
PRINT "r="; Geom.CircleR(c)
PRINT "r2="; Geom.CircleR(c2)
END
