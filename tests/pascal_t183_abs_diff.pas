{ Test absolute difference }
program TestAbsDiff;

function absDiff(a, b: integer): integer;
begin
  if a > b then
    absDiff := a - b
  else
    absDiff := b - a;
end;

begin
  writeln(absDiff(10, 3));
  writeln(absDiff(3, 10));
  writeln(absDiff(5, 5));
  writeln(absDiff(0, 100));
  writeln(absDiff(-5, 5));
end.
