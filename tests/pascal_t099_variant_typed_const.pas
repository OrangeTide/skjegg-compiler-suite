program t099_variant_typed_const;
type
  TShape = record
    x, y: integer;
    case kind: integer of
      1: (radius: integer);
      2: (width, height: integer)
  end;
const
  circle: TShape = (x: 5; y: 10; kind: 1; radius: 7);
  rect: TShape = (x: 1; y: 2; kind: 2; width: 3; height: 4);
begin
  writeln(circle.x);
  writeln(circle.y);
  writeln(circle.kind);
  writeln(circle.radius);
  writeln(rect.x);
  writeln(rect.y);
  writeln(rect.kind);
  writeln(rect.width);
  writeln(rect.height);
end.
