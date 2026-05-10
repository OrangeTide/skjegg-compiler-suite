{$Q+}
program OverflowOk;
var
  a, b: integer;
begin
  a := 100;
  b := 200;
  writeln(a + b);
  writeln(a - b);
  writeln(a * b)
end.
