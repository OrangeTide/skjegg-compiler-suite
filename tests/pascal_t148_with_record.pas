{ Test with statement on records }
program TestWithRecord;
type
  Point = record
    x, y: integer;
  end;
var
  p: Point;
begin
  with p do begin
    x := 10;
    y := 20;
  end;
  writeln(p.x);
  writeln(p.y);

  { modify via with }
  with p do begin
    x := x + 5;
    y := y * 2;
    writeln(x);
    writeln(y);
  end;

  { sum }
  with p do
    writeln(x + y);
end.
