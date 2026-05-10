{ Test complex while loops }
program TestWhileComplex;
var
  n, sum, count: integer;
begin
  { sum of 1..100 }
  n := 1;
  sum := 0;
  while n <= 100 do begin
    sum := sum + n;
    n := n + 1;
  end;
  writeln(sum);

  { count digits }
  n := 123456;
  count := 0;
  while n > 0 do begin
    n := n div 10;
    count := count + 1;
  end;
  writeln(count);

  { collatz steps from 27 }
  n := 27;
  count := 0;
  while n <> 1 do begin
    if odd(n) then
      n := 3 * n + 1
    else
      n := n div 2;
    count := count + 1;
  end;
  writeln(count);

  { while false - zero iterations }
  n := 0;
  while false do
    n := n + 1;
  writeln(n);
end.
