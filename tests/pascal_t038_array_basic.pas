program t038_array_basic;
type
  TArr = array[1..5] of integer;
var
  a: TArr;
  i: integer;
begin
  for i := 1 to 5 do
    a[i] := i * 10;
  for i := 1 to 5 do
    writeln(a[i]);
end.
