{ Test global arrays }
program TestGlobalArray;
var
  a: array[1..5] of integer;
  i, tmp: integer;
begin
  for i := 1 to 5 do
    a[i] := i * i;

  for i := 1 to 5 do begin
    if i > 1 then write(' ');
    write(a[i]);
  end;
  writeln;

  { reverse }
  for i := 1 to 2 do begin
    tmp := a[i];
    a[i] := a[6 - i];
    a[6 - i] := tmp;
  end;

  for i := 1 to 5 do begin
    if i > 1 then write(' ');
    write(a[i]);
  end;
  writeln;
end.
