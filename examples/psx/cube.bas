REM cube.bas - a spinning 3D cube in BASIC through the psx module's GTE helpers.
REM
REM The BASIC counterpart of cube.pas and cube.kp. The pointer-free GTE helpers
REM (GteRotXyz, GteProject, GteSx/Sy/Sz) take and return plain integers, so BASIC
REM needs no matrix or vertex arrays at the boundary: build a rotation from two
REM angles, project each corner, read back its screen x, y, and depth. The six
REM faces go into the ordering table at their average depth, so one swap draws
REM them back to front. BASIC has no array initializers, so the geometry is filled
REM in once before the loop.
IMPORT psx

DIM V(7, 2) AS INT          REM eight corners, each x, y, z
DIM F(5, 3) AS INT          REM six faces, each four corner indices
DIM CR(5) AS INT
DIM CG(5) AS INT
DIM CB(5) AS INT

REM corners
V(0, 0) = -40 : V(0, 1) = -40 : V(0, 2) = -40
V(1, 0) =  40 : V(1, 1) = -40 : V(1, 2) = -40
V(2, 0) =  40 : V(2, 1) =  40 : V(2, 2) = -40
V(3, 0) = -40 : V(3, 1) =  40 : V(3, 2) = -40
V(4, 0) = -40 : V(4, 1) = -40 : V(4, 2) =  40
V(5, 0) =  40 : V(5, 1) = -40 : V(5, 2) =  40
V(6, 0) =  40 : V(6, 1) =  40 : V(6, 2) =  40
V(7, 0) = -40 : V(7, 1) =  40 : V(7, 2) =  40

REM faces (corner indices, wound so the quad order is a, b, d, c)
F(0, 0) = 4 : F(0, 1) = 5 : F(0, 2) = 6 : F(0, 3) = 7
F(1, 0) = 1 : F(1, 1) = 0 : F(1, 2) = 3 : F(1, 3) = 2
F(2, 0) = 0 : F(2, 1) = 4 : F(2, 2) = 7 : F(2, 3) = 3
F(3, 0) = 5 : F(3, 1) = 1 : F(3, 2) = 2 : F(3, 3) = 6
F(4, 0) = 0 : F(4, 1) = 1 : F(4, 2) = 5 : F(4, 3) = 4
F(5, 0) = 3 : F(5, 1) = 7 : F(5, 2) = 6 : F(5, 3) = 2

REM face colors
CR(0) = 255 : CG(0) =   0 : CB(0) =   0
CR(1) =   0 : CG(1) = 255 : CB(1) =   0
CR(2) =   0 : CG(2) =   0 : CB(2) = 255
CR(3) = 255 : CG(3) = 255 : CB(3) =   0
CR(4) = 255 : CG(4) =   0 : CB(4) = 255
CR(5) =   0 : CG(5) = 255 : CB(5) = 255

DIM frame AS INT
DIM fi AS INT
DIM angle AS INT
DIM ia AS INT
DIM ib AS INT
DIM ic AS INT
DIM id AS INT
DIM z AS INT
DIM x0 AS INT
DIM y0 AS INT
DIM x1 AS INT
DIM y1 AS INT
DIM x2 AS INT
DIM y2 AS INT
DIM x3 AS INT
DIM y3 AS INT

psx.GpuInit()
psx.IrqInit()
psx.SetBg(0, 0, 20)
psx.GteSetScreen(300, 160, 120)
angle = 0

FOR frame = 0 TO 179
  psx.GteRotXyz(angle DIV 2, angle, 0)
  psx.GteSetTrans(0, 0, 220)
  psx.OtClear()

  FOR fi = 0 TO 5
    ia = F(fi, 0) : ib = F(fi, 1) : ic = F(fi, 2) : id = F(fi, 3)

    psx.GteProject(V(ia, 0), V(ia, 1), V(ia, 2))
    x0 = psx.GteSx() : y0 = psx.GteSy() : z = psx.GteSz()
    psx.GteProject(V(ib, 0), V(ib, 1), V(ib, 2))
    x1 = psx.GteSx() : y1 = psx.GteSy() : z = z + psx.GteSz()
    psx.GteProject(V(id, 0), V(id, 1), V(id, 2))
    x2 = psx.GteSx() : y2 = psx.GteSy() : z = z + psx.GteSz()
    psx.GteProject(V(ic, 0), V(ic, 1), V(ic, 2))
    x3 = psx.GteSx() : y3 = psx.GteSy() : z = z + psx.GteSz()

    psx.OtAddPoly4(x0, y0, x1, y1, x2, y2, x3, y3, CR(fi), CG(fi), CB(fi), z DIV 4)
  NEXT fi

  psx.OtLink()
  psx.Vsync()
  psx.DisplaySwap()
  angle = (angle + 4) BAND 1023
NEXT frame
END
