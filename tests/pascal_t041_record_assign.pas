program t041_record_assign;
type
  TPoint = record
    x: integer;
    y: integer;
  end;
var
  a, b: TPoint;
begin
  a.x := 10;
  a.y := 20;
  b := a;
  b.x := b.x + 1;
  writeln(a.x, ' ', a.y);
  writeln(b.x, ' ', b.y);
end.
