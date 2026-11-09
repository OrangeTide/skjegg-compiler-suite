REM consume Geom's opaque TYPE: import the handle type and its accessors, hold
REM a handle in a variable, construct through the module, read and mutate it
REM only through the exported routines (the fields are never visible here)
FROM Geom IMPORT Circle, NewCircle, CircleR, CircleDiameter, SetCircleR

DIM c AS Circle
c = NewCircle(1.0, 2.0, 5.0)
PRINT "r="; CircleR(c)
PRINT "d="; CircleDiameter(c)

DIM c2 AS Circle
c2 = NewCircle(0.0, 0.0, 100.0)
SetCircleR(c, 12.5)
PRINT "r="; CircleR(c)
PRINT "r2="; CircleR(c2)
END
