{ Test linear search in array }
program TestArraySearch;
var
  a: array[1..8] of integer;
  i, target, found: integer;
begin
  a[1] := 15; a[2] := 23; a[3] := 42;
  a[4] := 8; a[5] := 67; a[6] := 31;
  a[7] := 99; a[8] := 4;

  { Search for 42 }
  target := 42;
  found := 0;
  for i := 1 to 8 do
    if (found = 0) and then (a[i] = target) then
      found := i;
  writeln(found);

  { Search for 99 }
  target := 99;
  found := 0;
  for i := 1 to 8 do
    if (found = 0) and then (a[i] = target) then
      found := i;
  writeln(found);

  { Search for 50 - not found }
  target := 50;
  found := 0;
  for i := 1 to 8 do
    if (found = 0) and then (a[i] = target) then
      found := i;
  writeln(found);
end.
