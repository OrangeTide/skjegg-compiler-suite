{ Test multiple procedures }
program TestMultipleProc;
var
  result: integer;

function add(a, b: integer): integer;
begin
  add := a + b;
end;

function multiply(a, b: integer): integer;
begin
  multiply := a * b;
end;

function square(n: integer): integer;
begin
  square := multiply(n, n);
end;

function cube(n: integer): integer;
begin
  cube := multiply(square(n), n);
end;

begin
  writeln(add(3, 4));
  writeln(multiply(5, 6));
  writeln(square(7));
  writeln(cube(3));
  writeln(add(multiply(2, 3), multiply(4, 5)));
end.
