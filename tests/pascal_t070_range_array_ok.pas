{$R+}
program RangeArrayOk;
var
  a: array[1..5] of integer;
  i: integer;
begin
  for i := 1 to 5 do
    a[i] := i * 10;
  writeln(a[1]);
  writeln(a[3]);
  writeln(a[5])
end.
