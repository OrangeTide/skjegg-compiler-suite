program reals;
var x, y: real; n: integer; b: boolean;
procedure emitf(v: real); external;
function scale(a: real; k: integer): real;
begin
  scale := a * k + 1.5;
end;
begin
  n := 3;
  x := 2.5;
  y := x / 2.0 + n;
  b := (x > y) or (y = 1.5);
  emitf(scale(y, n));
end.
