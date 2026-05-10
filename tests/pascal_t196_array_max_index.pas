{ Test finding index of maximum element }
program TestMaxIndex;
var
  a: array[1..8] of integer;
  i, maxIdx: integer;
begin
  a[1] := 12; a[2] := 45; a[3] := 7;
  a[4] := 89; a[5] := 23; a[6] := 56;
  a[7] := 3; a[8] := 67;

  maxIdx := 1;
  for i := 2 to 8 do
    if a[i] > a[maxIdx] then
      maxIdx := i;

  writeln(maxIdx);
  writeln(a[maxIdx]);
end.
