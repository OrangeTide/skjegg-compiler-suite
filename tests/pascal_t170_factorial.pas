{ Test factorial: iterative and recursive }
program TestFactorial;

function fact_iter(n: integer): integer;
var
  result, i: integer;
begin
  result := 1;
  for i := 2 to n do
    result := result * i;
  fact_iter := result;
end;

function fact_rec(n: integer): integer;
begin
  if n <= 1 then
    fact_rec := 1
  else
    fact_rec := n * fact_rec(n - 1);
end;

begin
  writeln(fact_iter(0));
  writeln(fact_iter(1));
  writeln(fact_iter(5));
  writeln(fact_iter(10));
  writeln(fact_rec(5));
  writeln(fact_rec(10));
end.
