{ Test nested records }
program TestNestedRecord;
type
  Point = record
    x, y: integer;
  end;
  Rect = record
    tl, br: Point;
  end;
var
  r: Rect;
  area: integer;
begin
  r.tl.x := 10;
  r.tl.y := 20;
  r.br.x := 50;
  r.br.y := 80;

  area := (r.br.x - r.tl.x) * (r.br.y - r.tl.y);
  writeln(area);

  writeln(r.tl.x);
  writeln(r.tl.y);
  writeln(r.br.x);
  writeln(r.br.y);
end.
