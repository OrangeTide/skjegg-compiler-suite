program recretnoarg;
{ Regression: a no-argument function returning a record by value, called bare
  (no parentheses), must lower as a call that returns through an sret buffer,
  exactly like the parenthesized form. Before the fix the bare call was addressed
  as if it were the current function's own result slot, so no call was emitted and
  the record came back as garbage. Exercise the bare call in every address context
  the fix touches: an assignment, a field read, and a by-const-reference argument. }
type Point = record x, y: integer end;

function origin: Point;
begin
  origin.x := 3;
  origin.y := 4;
end;

function sumPt(const p: Point): integer;
begin
  sumPt := p.x + p.y;
end;

var a, b: Point;
begin
  a := origin;                       { bare call, the regressed path }
  b := origin();                     { parenthesized, the always-working path }
  WriteLn('a=', a.x, ',', a.y);
  WriteLn('b=', b.x, ',', b.y);
  WriteLn('barefield=', origin.x);   { bare call, then field read }
  WriteLn('sum=', sumPt(origin));    { bare call passed by const-reference }
end.
