{ Bare random returns a real in [0, 1); randomize reseeds (its effect is not
  pinned here, only that it runs).  Host only (reals). }
program RndReal;
var i: integer; r, lo, hi: real;
begin
  randomize;
  lo := 2.0; hi := -1.0;
  for i := 1 to 2000 do
  begin
    r := random;
    if r < lo then lo := r;
    if r > hi then hi := r
  end;
  if (lo >= 0.0) and (hi < 1.0) and (lo < hi) then
    writeln('ok')
  else
    writeln('fail')
end.
