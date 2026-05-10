{ Test Fibonacci sequence }
program TestFibonacci;
var
  a, b, tmp, i: integer;

function fib_recursive(n: integer): integer;
begin
  if n <= 1 then
    fib_recursive := n
  else
    fib_recursive := fib_recursive(n - 1) + fib_recursive(n - 2);
end;

begin
  { iterative }
  a := 0;
  b := 1;
  for i := 1 to 10 do begin
    if i > 1 then write(' ');
    write(a);
    tmp := a + b;
    a := b;
    b := tmp;
  end;
  writeln;

  { recursive }
  writeln(fib_recursive(10));
  writeln(fib_recursive(20));
end.
