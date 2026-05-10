{ Test record field comparison }
program TestRecordCompare;
type
  Point = record
    x, y: integer;
  end;
var
  a, b: Point;

function pointsEqual(var p, q: Point): boolean;
begin
  pointsEqual := (p.x = q.x) and (p.y = q.y);
end;

begin
  a.x := 5; a.y := 10;
  b.x := 5; b.y := 10;
  if pointsEqual(a, b) then writeln('equal');

  b.y := 20;
  if not pointsEqual(a, b) then writeln('not equal');
end.
