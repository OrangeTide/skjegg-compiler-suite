{ Test selection sort }
program TestSelectionSort;
var
  a: array[1..6] of integer;
  i, j, minIdx, tmp: integer;
begin
  a[1] := 42; a[2] := 17; a[3] := 93;
  a[4] := 5; a[5] := 71; a[6] := 28;

  for i := 1 to 5 do begin
    minIdx := i;
    for j := i + 1 to 6 do
      if a[j] < a[minIdx] then
        minIdx := j;
    if minIdx <> i then begin
      tmp := a[i];
      a[i] := a[minIdx];
      a[minIdx] := tmp;
    end;
  end;

  for i := 1 to 6 do begin
    if i > 1 then write(' ');
    write(a[i]);
  end;
  writeln;
end.
