{ Test insertion sort }
program TestInsertionSort;
var
  a: array[1..7] of integer;
  i, j, key: integer;
begin
  a[1] := 38; a[2] := 27; a[3] := 43;
  a[4] := 3; a[5] := 9; a[6] := 82; a[7] := 10;

  for i := 2 to 7 do begin
    key := a[i];
    j := i - 1;
    while (j >= 1) and (a[j] > key) do begin
      a[j + 1] := a[j];
      j := j - 1;
    end;
    a[j + 1] := key;
  end;

  for i := 1 to 7 do begin
    if i > 1 then write(' ');
    write(a[i]);
  end;
  writeln;
end.
