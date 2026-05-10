program t042_array_assign;
type
  TArr = array[1..5] of integer;
var
  a, b: TArr;
  i: integer;
begin
  for i := 1 to 5 do
    a[i] := i * 10;
  b := a;
  b[3] := 99;
  write(a[1]);
  for i := 2 to 5 do
    write(' ', a[i]);
  writeln;
  write(b[1]);
  for i := 2 to 5 do
    write(' ', b[i]);
  writeln;
end.
