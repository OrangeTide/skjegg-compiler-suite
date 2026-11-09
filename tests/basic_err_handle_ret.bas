REM Returning a Square handle from an FN declared to return a Circle must fail.
MODULE HErrRet
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
EXPORT FN GetCircle(s AS FLOAT) AS Circle
    DIM sq AS Square
    sq = NewSquare(s)
    RETURN sq
ENDFN
