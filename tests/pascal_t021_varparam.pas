program testvarparam;
{ Test: var parameters (pass by reference). }

procedure Swap(var a, b: integer);
var tmp: integer;
begin
  tmp := a;
  a := b;
  b := tmp
end;

procedure DoubleIt(var x: integer);
begin
  x := x * 2
end;

var p, q: integer;
begin
  p := 10;
  q := 20;
  Swap(p, q);
  writeln(p);
  writeln(q);
  DoubleIt(p);
  writeln(p)
end.
