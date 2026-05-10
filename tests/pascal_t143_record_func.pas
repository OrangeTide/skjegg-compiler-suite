{ Test records with functions }
program TestRecordFunc;
type
  Point = record
    x, y: integer;
  end;
var
  p1, p2: Point;

function distance_sq(var a, b: Point): integer;
var
  dx, dy: integer;
begin
  dx := b.x - a.x;
  dy := b.y - a.y;
  distance_sq := dx * dx + dy * dy;
end;

procedure move(var p: Point; dx, dy: integer);
begin
  p.x := p.x + dx;
  p.y := p.y + dy;
end;

begin
  p1.x := 0; p1.y := 0;
  p2.x := 3; p2.y := 4;
  writeln(distance_sq(p1, p2));

  move(p1, 10, 20);
  writeln(p1.x);
  writeln(p1.y);

  move(p2, -1, -1);
  writeln(p2.x);
  writeln(p2.y);
  writeln(distance_sq(p1, p2));
end.
