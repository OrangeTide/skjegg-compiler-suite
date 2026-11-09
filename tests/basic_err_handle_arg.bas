REM A Square handle passed where a Circle handle is expected must be rejected.
MODULE HErrArg
EXPORT TYPE Circle
    r AS FLOAT
ENDTYPE
EXPORT TYPE Square
    side AS FLOAT
ENDTYPE
EXPORT FN NewSquare(s AS FLOAT) AS Square
    DIM sq AS Square
    sq.side = s
    RETURN sq
ENDFN
FN CircleArea(c AS Circle) AS FLOAT
    RETURN c.r
ENDFN
EXPORT FN Oops(s AS FLOAT) AS FLOAT
    DIM sq AS Square
    sq = NewSquare(s)
    RETURN CircleArea(sq)
ENDFN
