program constagg;
type
  Pt = record x, y: integer; end;
  Seg = record a, b: Pt; w: real; name: string; end;
const
  grid: array[1..2, 1..3] of integer = ((1, 2, 3), (4, 5, 6));
  pts: array[0..1] of Pt = ((x: 1; y: 2), (x: 3; y: 4));
  s: Seg = (a: (x: 10; y: 20); b: (x: 30; y: 40); w: 2.5; name: 'hi');
  big: array[1..2] of int64 = (5000000000, 7);
procedure emit(v: integer); external;
procedure emitf(v: real); external;
procedure emit64(v: int64); external;
begin
  emit(grid[2, 3]);
  emit(grid[1, 2]);
  emit(pts[1].x + pts[1].y);
  emit(s.a.x + s.b.y);
  emitf(s.w);
  writeln(s.name);
  emit64(big[1]);
end.
