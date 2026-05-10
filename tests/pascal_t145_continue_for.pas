{ Test continue in for loop }
program TestContinueFor;
var
  i, sum, count: integer;
begin
  { sum of odd numbers 1..20 }
  sum := 0;
  for i := 1 to 20 do begin
    if i mod 2 = 0 then continue;
    sum := sum + i;
  end;
  writeln(sum);

  { skip multiples of 3, count printed }
  count := 0;
  for i := 1 to 15 do begin
    if i mod 3 = 0 then continue;
    if count > 0 then write(' ');
    write(i);
    count := count + 1;
  end;
  writeln;
end.
