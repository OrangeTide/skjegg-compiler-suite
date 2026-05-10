{ Test comparison chains }
program TestCompChain;
var
  a, b, c: integer;
begin
  a := 1; b := 2; c := 3;
  if (a < b) and (b < c) then writeln('ascending');
  if (a <= b) and (b <= c) then writeln('non-descending');

  a := 3; b := 2; c := 1;
  if (a > b) and (b > c) then writeln('descending');

  a := 5; b := 5; c := 5;
  if (a = b) and (b = c) then writeln('all equal');
  if (a >= b) and (b >= c) then writeln('non-ascending');

  { mixed }
  a := 10; b := 20;
  if a <> b then writeln('different');
  if not (a = b) then writeln('not equal');
end.
