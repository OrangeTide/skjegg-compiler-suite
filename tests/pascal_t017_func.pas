program TestFunc;
{ Test: functions with parameters and return values. }

function Double(x: integer): integer;
begin
  Double := x * 2
end;

function Add(a, b: integer): integer;
begin
  Add := a + b
end;

function Factorial(n: integer): integer;
var result: integer;
    i: integer;
begin
  result := 1;
  for i := 2 to n do
    result := result * i;
  Factorial := result
end;

begin
  writeln(Double(21));
  writeln(Add(100, 200));
  writeln(Factorial(5));
  writeln(Double(Add(3, 4)))
end.
