{ Test recursive digit sum }
program TestSumDigitsRec;

function digitSum(n: integer): integer;
begin
  if n < 10 then
    digitSum := n
  else
    digitSum := (n mod 10) + digitSum(n div 10);
end;

begin
  writeln(digitSum(0));
  writeln(digitSum(5));
  writeln(digitSum(123));
  writeln(digitSum(999));
  writeln(digitSum(10000));
end.
