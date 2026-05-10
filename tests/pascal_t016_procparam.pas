program TestProcParam;
{ Test: procedure with value parameters. }

procedure PrintSum(a, b: integer);
begin
  writeln(a + b)
end;

procedure PrintTriple(x: integer);
begin
  writeln(x * 3)
end;

begin
  PrintSum(10, 20);
  PrintTriple(7);
  PrintSum(100, 200)
end.
