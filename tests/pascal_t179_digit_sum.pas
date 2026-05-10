{ Test digit sum }
program TestDigitSum;

function digitSum(n: integer): integer;
var
  sum: integer;
begin
  if n < 0 then n := -n;
  sum := 0;
  while n > 0 do begin
    sum := sum + n mod 10;
    n := n div 10;
  end;
  digitSum := sum;
end;

begin
  writeln(digitSum(0));
  writeln(digitSum(123));
  writeln(digitSum(9999));
  writeln(digitSum(100000));
  writeln(digitSum(-456));
end.
