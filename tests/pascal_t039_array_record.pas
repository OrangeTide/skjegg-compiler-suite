program t039_array_record;
type
  TPoint = record
    x: integer;
    y: integer;
  end;
  TPoints = array[0..2] of TPoint;
var
  pts: TPoints;
  i: integer;
begin
  for i := 0 to 2 do begin
    pts[i].x := i * 2;
    pts[i].y := i * 2 + 1;
  end;
  for i := 0 to 2 do
    writeln(pts[i].x, ' ', pts[i].y);
end.
