{ Test max/min functions }
program TestMaxMin;
var
  a: array[1..6] of integer;
  i, maxVal, minVal: integer;
begin
  a[1] := 42; a[2] := 17; a[3] := 93;
  a[4] := 5; a[5] := 71; a[6] := 28;

  maxVal := a[1];
  minVal := a[1];
  for i := 2 to 6 do begin
    if a[i] > maxVal then maxVal := a[i];
    if a[i] < minVal then minVal := a[i];
  end;

  writeln(maxVal);
  writeln(minVal);
  writeln(maxVal - minVal);
end.
