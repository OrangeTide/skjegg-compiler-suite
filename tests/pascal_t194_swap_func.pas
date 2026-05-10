{ Test swap using var parameters }
program TestSwap;
var
  a, b: integer;

procedure swap(var x, y: integer);
var
  tmp: integer;
begin
  tmp := x;
  x := y;
  y := tmp;
end;

begin
  a := 10;
  b := 20;
  swap(a, b);
  writeln(a);
  writeln(b);

  a := -5;
  b := 100;
  swap(a, b);
  writeln(a);
  writeln(b);
end.
