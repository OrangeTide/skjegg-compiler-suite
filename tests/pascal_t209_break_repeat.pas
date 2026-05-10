{ Test break in repeat-until loops }
program TestBreakRepeat;
var
  i, sum: integer;
begin
  { Break when sum exceeds 50 }
  sum := 0;
  i := 1;
  repeat
    sum := sum + i * i;
    if sum > 50 then
      break;
    i := i + 1;
  until i > 100;
  writeln(i);
  writeln(sum);

  { Find first multiple of 7 above 20 }
  i := 21;
  repeat
    if i mod 7 = 0 then
      break;
    i := i + 1;
  until i > 100;
  writeln(i);
end.
