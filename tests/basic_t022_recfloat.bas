REM FLOAT fields at nonzero record offsets stay aligned and exact.
REM The doubles are exact binary fractions, so the formatter prints them
REM exactly on every backend (hardware fld/fsd on rv32/arm64, SSE on x86-64,
REM soft-float on ColdFire/MIPS); a misaligned field would corrupt the value.

TYPE Mix
    tag AS BYTE
    scale AS FLOAT
    num AS INT
    ratio AS FLOAT
ENDTYPE

DIM m AS Mix
m.tag = 7
m.scale = 3.25
m.num = 1000000
m.ratio = 0.015625
PRINT m.tag
PRINT m.scale
PRINT m.num
PRINT m.ratio

REM whole-record copy preserves every field, doubles included
DIM n AS Mix
n = m
n.scale = 2.5
PRINT m.scale
PRINT n.scale
PRINT n.ratio

REM arithmetic on a float field read back from the record
m.scale = m.scale + m.ratio
PRINT m.scale

END
