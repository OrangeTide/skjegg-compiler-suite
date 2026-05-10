{ Test counting elements matching criteria }
program TestArrayCount;
var
  a: array[1..10] of integer;
  i, count: integer;
begin
  a[1] := 3; a[2] := 7; a[3] := 2; a[4] := 9; a[5] := 1;
  a[6] := 8; a[7] := 4; a[8] := 6; a[9] := 5; a[10] := 10;

  { Count elements > 5 }
  count := 0;
  for i := 1 to 10 do
    if a[i] > 5 then
      count := count + 1;
  writeln(count);

  { Count even elements }
  count := 0;
  for i := 1 to 10 do
    if a[i] mod 2 = 0 then
      count := count + 1;
  writeln(count);

  { Count elements between 3 and 7 inclusive }
  count := 0;
  for i := 1 to 10 do
    if (a[i] >= 3) and (a[i] <= 7) then
      count := count + 1;
  writeln(count);
end.
