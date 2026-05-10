{ Test var parameter with array element }
program TestVarParamArray;
var
  a: array[1..5] of integer;
  i: integer;

procedure triple(var x: integer);
begin
  x := x * 3;
end;

begin
  for i := 1 to 5 do
    a[i] := i * 10;

  for i := 1 to 5 do
    triple(a[i]);

  for i := 1 to 5 do begin
    if i > 1 then write(' ');
    write(a[i]);
  end;
  writeln;
end.
