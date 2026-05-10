{ Test passing records to procedures }
program TestRecordParam;
type
  Vec2 = record
    x, y: integer;
  end;
var
  a, b: Vec2;

function dot(var u, v: Vec2): integer;
begin
  dot := u.x * v.x + u.y * v.y;
end;

procedure scale(var v: Vec2; factor: integer);
begin
  v.x := v.x * factor;
  v.y := v.y * factor;
end;

begin
  a.x := 3; a.y := 4;
  b.x := 1; b.y := 2;

  writeln(dot(a, b));

  scale(a, 2);
  writeln(a.x);
  writeln(a.y);

  writeln(dot(a, b));
end.
