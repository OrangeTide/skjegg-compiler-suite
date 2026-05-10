{ Test array of records }
program TestArrayOfRecord;
type
  Point = record
    x, y: integer;
  end;
var
  pts: array[1..3] of Point;
  i: integer;
begin
  pts[1].x := 10; pts[1].y := 20;
  pts[2].x := 30; pts[2].y := 40;
  pts[3].x := 50; pts[3].y := 60;

  for i := 1 to 3 do
    writeln(pts[i].x, ' ', pts[i].y);

  { modify through index }
  pts[2].x := pts[2].x + 5;
  pts[2].y := pts[2].y - 5;
  writeln(pts[2].x, ' ', pts[2].y);
end.
