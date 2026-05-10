{ Test boolean variables and writing }
program TestBooleanWrite;
var
  a, b, c: boolean;
  x: integer;
begin
  a := true;
  b := false;
  c := a and b;
  writeln(ord(a));
  writeln(ord(b));
  writeln(ord(c));

  c := a or b;
  writeln(ord(c));

  c := not a;
  writeln(ord(c));

  { boolean from comparison }
  x := 42;
  a := x > 10;
  b := x < 10;
  writeln(ord(a));
  writeln(ord(b));
end.
