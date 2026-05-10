{ Test boolean flags as counters }
program TestFlagCounter;
var
  arr: array[1..10] of integer;
  i, pos, neg, zero: integer;
begin
  arr[1] := 5; arr[2] := -3; arr[3] := 0;
  arr[4] := 7; arr[5] := -1; arr[6] := -8;
  arr[7] := 0; arr[8] := 12; arr[9] := -4;
  arr[10] := 6;

  pos := 0; neg := 0; zero := 0;
  for i := 1 to 10 do begin
    if arr[i] > 0 then
      pos := pos + 1
    else if arr[i] < 0 then
      neg := neg + 1
    else
      zero := zero + 1;
  end;

  writeln(pos);
  writeln(neg);
  writeln(zero);
end.
