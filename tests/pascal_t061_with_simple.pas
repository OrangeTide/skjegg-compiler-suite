program WithSimple;
type
  TPoint = record
    x, y: integer;
  end;
var
  p: TPoint;
begin
  p.x := 10;
  p.y := 20;
  with p do begin
    writeln(x);
    writeln(y);
    x := 30;
    y := 40;
  end;
  writeln(p.x);
  writeln(p.y)
end.
