program TestForward;
{ Test: forward declarations. }

procedure PrintResult(x: integer); forward;

function Compute(a, b: integer): integer;
begin
  Compute := a * b + 1
end;

procedure PrintResult(x: integer);
begin
  writeln(x)
end;

begin
  PrintResult(Compute(6, 7))
end.
