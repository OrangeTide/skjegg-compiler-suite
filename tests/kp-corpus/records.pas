program agg;
type
  Point = record x, y: integer; end;
  Line = record a, b: Point; len: real; end;
var
  p: Point;
  grid: array[0..3, 1..2] of integer;
  seg: Line;
  i: integer;
procedure emit(v: integer); external;
begin
  p.x := 10;
  p.y := p.x + 5;
  seg.a := p;
  seg.b.x := 99;
  seg.len := 2.5;
  for i := 0 to 3 do
    grid[i, 1] := i * i;
  emit(grid[2, 1] + seg.b.x + seg.a.y);
end.
