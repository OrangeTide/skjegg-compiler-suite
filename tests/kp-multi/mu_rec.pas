program mu_rec;
{ a record type exported from a unit, with value-copy semantics across an
  assignment and a const-parameter accessor. }
uses recdep as r;
var
  p, q: r.Point;
begin
  p := r.Make(3, 4, 'origin');
  q := p;
  WriteLn('q.x=', q.x, ' q.y=', q.y);
  WriteLn('q.tag=', q.tag);
  WriteLn('SumXY(p)=', r.SumXY(p));
  WriteLn('sum=', q.x + q.y + r.SumXY(p));
end.
