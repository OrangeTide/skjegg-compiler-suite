{ Test record field initialization and access }
program TestRecordInit;
type
  Point = record
    x, y: integer;
  end;
var
  pts: array[1..4] of Point;
  i, sumx, sumy: integer;
begin
  pts[1].x := 1; pts[1].y := 2;
  pts[2].x := 3; pts[2].y := 4;
  pts[3].x := 5; pts[3].y := 6;
  pts[4].x := 7; pts[4].y := 8;

  sumx := 0;
  sumy := 0;
  for i := 1 to 4 do begin
    sumx := sumx + pts[i].x;
    sumy := sumy + pts[i].y;
  end;
  writeln(sumx);
  writeln(sumy);

  { Swap x and y of first point }
  i := pts[1].x;
  pts[1].x := pts[1].y;
  pts[1].y := i;
  writeln(pts[1].x);
  writeln(pts[1].y);
end.
