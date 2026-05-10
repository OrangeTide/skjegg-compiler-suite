{ Test array rotation }
program TestArrayRotate;
var
  a: array[1..5] of integer;
  i, first: integer;
begin
  for i := 1 to 5 do
    a[i] := i * 10;

  { rotate left by 2 }
  first := a[1];
  a[1] := a[2];
  a[2] := a[3];
  a[3] := a[4];
  a[4] := a[5];
  a[5] := first;

  first := a[1];
  a[1] := a[2];
  a[2] := a[3];
  a[3] := a[4];
  a[4] := a[5];
  a[5] := first;

  for i := 1 to 5 do begin
    if i > 1 then write(' ');
    write(a[i]);
  end;
  writeln;
end.
