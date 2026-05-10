{ Test repeat-until with complex conditions }
program TestRepeatComplex;
var
  i, sum: integer;
begin
  { sum 1..10 using repeat }
  i := 1;
  sum := 0;
  repeat
    sum := sum + i;
    inc(i);
  until i > 10;
  writeln(sum);

  { find first multiple of 7 > 50 }
  i := 50;
  repeat
    inc(i);
  until (i mod 7 = 0);
  writeln(i);

  { nested repeat }
  i := 0;
  sum := 0;
  repeat
    i := i + 1;
    repeat
      sum := sum + 1;
    until sum mod 3 = 0;
  until i >= 4;
  writeln(sum);
end.
