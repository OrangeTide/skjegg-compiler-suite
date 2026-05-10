{ Test nested if-then-else }
program TestNestedIf;
var
  a, b, c: integer;
begin
  a := 5; b := 10; c := 3;

  { three-way max }
  if a >= b then begin
    if a >= c then
      writeln(a)
    else
      writeln(c);
  end else begin
    if b >= c then
      writeln(b)
    else
      writeln(c);
  end;

  { classify number }
  a := -7;
  if a > 0 then
    writeln('positive')
  else if a < 0 then
    writeln('negative')
  else
    writeln('zero');

  { nested conditions }
  a := 15;
  if a > 10 then
    if a < 20 then
      if a mod 3 = 0 then
        writeln('divisible by 3')
      else
        writeln('not divisible')
    else
      writeln('too big')
  else
    writeln('too small');

  { dangling else }
  a := 5;
  if a > 0 then
    if a > 10 then
      writeln('big')
    else
      writeln('small');
end.
