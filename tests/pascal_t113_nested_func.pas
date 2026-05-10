{ Test nested functions accessing outer variables }
program TestNestedFunc;
var
  g: integer;

function Outer(x: integer): integer;
var
  y: integer;

  function Inner(a: integer): integer;
  begin
    Inner := a + y;
  end;

begin
  y := x * 2;
  Outer := Inner(x) + g;
end;

begin
  g := 100;
  writeln(Outer(5));
  writeln(Outer(10));
end.
