{ Test repeat-until }
program TestRepeatSimple;
var
  n, digits: integer;
begin
  { count digits of a number }
  n := 123456789;
  digits := 0;
  repeat
    digits := digits + 1;
    n := n div 10;
  until n = 0;
  writeln(digits);

  { sum 1..n using repeat }
  n := 1;
  digits := 0;
  repeat
    digits := digits + n;
    n := n + 1;
  until n > 50;
  writeln(digits);
end.
