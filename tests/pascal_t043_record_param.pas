program t043_record_param;
type
  TPoint = record
    x: integer;
    y: integer;
  end;

procedure PrintPoint(const p: TPoint);
begin
  writeln(p.x, ' ', p.y);
end;

procedure MovePoint(var p: TPoint; dx, dy: integer);
begin
  p.x := p.x + dx;
  p.y := p.y + dy;
end;

procedure DoublePoint(p: TPoint);
begin
  { Value param: modifications should not affect caller }
  p.x := p.x * 2;
  p.y := p.y * 2;
  writeln(p.x, ' ', p.y);
end;

var
  pt: TPoint;
begin
  pt.x := 3;
  pt.y := 4;
  PrintPoint(pt);
  MovePoint(pt, 10, 20);
  PrintPoint(pt);
  DoublePoint(pt);
  PrintPoint(pt);
end.
