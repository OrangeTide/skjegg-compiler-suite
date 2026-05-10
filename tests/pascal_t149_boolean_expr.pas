{ Test boolean expressions }
program TestBoolExpr;
var
  a, b, c: boolean;
  x: integer;
begin
  a := true;
  b := false;

  { basic logic }
  c := a and b;
  if c then writeln('T') else writeln('F');

  c := a or b;
  if c then writeln('T') else writeln('F');

  c := not a;
  if c then writeln('T') else writeln('F');

  c := not b;
  if c then writeln('T') else writeln('F');

  { compound }
  x := 15;
  if (x > 10) and (x < 20) then writeln('in range');
  if (x < 5) or (x > 10) then writeln('outside 5-10');
  if not ((x = 0) or (x = 1)) then writeln('not 0 or 1');

  { boolean from comparison }
  a := 5 > 3;
  b := 2 > 7;
  if a then writeln('5>3');
  if not b then writeln('not 2>7');

  { short circuit with and then }
  x := 0;
  if (x <> 0) and then (100 div x > 5) then
    writeln('ERROR')
  else
    writeln('safe');
end.
