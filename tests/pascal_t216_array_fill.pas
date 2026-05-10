{ Test array fill patterns }
program TestArrayFill;
var
  a: array[1..10] of integer;
  i: integer;
begin
  { Fill with constant }
  for i := 1 to 10 do
    a[i] := 42;
  writeln(a[1]);
  writeln(a[5]);
  writeln(a[10]);

  { Fill with index squared }
  for i := 1 to 10 do
    a[i] := i * i;
  for i := 1 to 5 do
    writeln(a[i]);

  { Fill alternating }
  for i := 1 to 10 do begin
    if i mod 2 = 0 then
      a[i] := 1
    else
      a[i] := 0;
  end;
  for i := 1 to 6 do
    write(a[i]);
  writeln;
end.
