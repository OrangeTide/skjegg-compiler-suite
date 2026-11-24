{ random(n) returns an integer in [0, n).  Without randomize the sequence
  starts from a fixed seed, so this is reproducible.  Draw many values and
  check they stay in range and are not all identical. }
program Rnd;
var i, r, mn, mx: integer;
begin
  mn := 100000;
  mx := -1;
  for i := 1 to 10000 do
  begin
    r := random(100);
    if r < mn then mn := r;
    if r > mx then mx := r
  end;
  if (mn >= 0) and (mx < 100) and (mn <> mx) then
    writeln('ok')
  else
    writeln('fail ', mn, ' ', mx)
end.
