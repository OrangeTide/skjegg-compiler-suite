REM Assigning a Square handle to a Circle handle variable must be rejected.
MODULE HErrAssign
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
EXPORT FN Oops(s AS FLOAT) AS FLOAT
    DIM c AS Circle
    c = NewSquare(s)
    RETURN c.r
ENDFN
