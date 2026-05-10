program t036_record_basic;
type
  TPoint = record
    x: integer;
    y: integer;
  end;
var
  p: TPoint;
begin
  p.x := 10;
  p.y := 20;
  writeln(p.x);
  writeln(p.y);
  writeln(p.x + p.y);
end.
