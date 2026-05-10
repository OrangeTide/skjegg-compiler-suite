{ Test repeat with countdown }
program TestRepeatCountdown;
var
  n, sum: integer;
begin
  { Countdown from 5 }
  n := 5;
  repeat
    writeln(n);
    n := n - 1;
  until n = 0;

  { Sum 1..10 with repeat }
  n := 1;
  sum := 0;
  repeat
    sum := sum + n;
    n := n + 1;
  until n > 10;
  writeln(sum);
end.
