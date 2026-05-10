program t037_record_nested;
type
  TPoint = record
    x: integer;
    y: integer;
  end;
  TRect = record
    tl: TPoint;
    br: TPoint;
  end;
var
  r: TRect;
begin
  r.tl.x := 1;
  r.tl.y := 2;
  r.br.x := 3;
  r.br.y := 4;
  writeln(r.tl.x);
  writeln(r.tl.y);
  writeln(r.br.x);
  writeln(r.br.y);
  writeln(r.br.x - r.tl.x);
end.
