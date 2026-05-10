program t098_variant_record;
type
  TShape = record
    x, y: integer;
    case kind: integer of
      1: (radius: integer);
      2: (width, height: integer)
  end;
var
  s: TShape;
begin
  s.x := 5;
  s.y := 10;
  s.kind := 1;
  s.radius := 7;
  writeln(s.x);
  writeln(s.y);
  writeln(s.kind);
  writeln(s.radius);

  s.kind := 2;
  s.width := 3;
  s.height := 4;
  writeln(s.kind);
  writeln(s.width);
  writeln(s.height);
end.
