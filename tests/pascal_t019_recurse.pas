program TestRecurse;
{ Test: recursive function call. }

function Fib(n: integer): integer;
begin
  if n < 2 then
    Fib := n
  else
    Fib := Fib(n - 1) + Fib(n - 2)
end;

begin
  writeln(Fib(0));
  writeln(Fib(1));
  writeln(Fib(5));
  writeln(Fib(10))
end.
