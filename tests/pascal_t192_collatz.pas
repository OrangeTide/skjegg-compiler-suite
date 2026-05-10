{ Test Collatz sequence length }
program TestCollatz;
var
  n, steps: integer;

function collatzLen(start: integer): integer;
var
  n, count: integer;
begin
  n := start;
  count := 0;
  while n <> 1 do begin
    if n mod 2 = 0 then
      n := n div 2
    else
      n := 3 * n + 1;
    count := count + 1;
  end;
  collatzLen := count;
end;

begin
  writeln(collatzLen(1));
  writeln(collatzLen(2));
  writeln(collatzLen(3));
  writeln(collatzLen(6));
  writeln(collatzLen(27));
end.
