REM hello.bas - the libpsx bouncing square in BASIC.
REM
REM The same demo as hello.pas and hello.kp: a yellow square bounces across a
REM dark blue background for 180 frames. It reaches the PlayStation through the
REM psx module (units/psx.bas), compiled with -I build/psx-bas and linked against
REM libpsx. `IMPORT psx` brings the entry points in under dotted names. This is
REM the BASIC front end driving the GPU, at parity with the C, Pascal, and Kobold
REM hello samples.
IMPORT psx

DIM f AS INT
DIM x AS INT
DIM dx AS INT

psx.GpuInit()
psx.IrqInit()
psx.SetBg(0, 0, 40)

x = 0
dx = 2
FOR f = 0 TO 179
  psx.DrawRect(x, 104, 32, 32, 255, 200, 0)
  psx.Vsync()
  psx.DisplaySwap()
  x = x + dx
  IF (x <= 0) OR (x >= 288) THEN dx = -dx
NEXT
END
