REM sprite.bas - the BASIC texture sample.
REM
REM The BASIC counterpart of sprite.pas and sprite.kp. A texture's raw TIM
REM pointer cannot cross into BASIC, so each texture is loaded by the shared C
REM accessor (examples/psx/sprite_asset.c), which uploads it and returns an
REM integer handle. BASIC declares those accessors EXTERNAL, then draws entirely
REM through the psx module: one texture as a plain sprite and one rotated. It
REM proves the BASIC front end reaches the richer 2D draws, the same as the C,
REM Pascal, and Kobold samples.
IMPORT psx

DECLARE FN LoadQuad() AS INT EXTERNAL "psx_load_quad"
DECLARE FN LoadSolid() AS INT EXTERNAL "psx_load_solid"

DIM f AS INT
DIM quad AS INT
DIM solid AS INT

psx.GpuInit()
psx.IrqInit()
psx.SetBg(0, 0, 20)

quad = LoadQuad()
solid = LoadSolid()

FOR f = 0 TO 119
  psx.TexDraw(quad, 40, 40)
  psx.TexRot(solid, 200, 120, 128, psx.FixOne)
  psx.Vsync()
  psx.DisplaySwap()
NEXT
END
