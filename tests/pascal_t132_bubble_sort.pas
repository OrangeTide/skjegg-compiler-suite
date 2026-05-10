{ Test bubble sort }
program TestBubbleSort;
var
  a: array[1..8] of integer;
  i, j, tmp: integer;
begin
  a[1] := 64; a[2] := 25; a[3] := 12; a[4] := 22;
  a[5] := 11; a[6] := 90; a[7] := 1; a[8] := 45;

  for i := 1 to 7 do
    for j := 1 to 8 - i do
      if a[j] > a[j + 1] then begin
        tmp := a[j];
        a[j] := a[j + 1];
        a[j + 1] := tmp;
      end;

  for i := 1 to 8 do begin
    if i > 1 then write(' ');
    write(a[i]);
  end;
  writeln;
end.
