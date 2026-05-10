program TypedConstRecord;
type
  TPoint = record
    x: integer;
    y: integer
  end;
const
  origin: TPoint = (x: 0; y: 0);
  p1: TPoint = (x: 3; y: 4);
begin
  writeln(origin.x);
  writeln(origin.y);
  writeln(p1.x);
  writeln(p1.y)
end.
