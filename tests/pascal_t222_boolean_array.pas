{ Test boolean array operations }
program TestBooleanArray;
var
  flags: array[1..10] of boolean;
  i, count: integer;
begin
  { Set even positions to true }
  for i := 1 to 10 do
    flags[i] := (i mod 2 = 0);

  { Count true flags }
  count := 0;
  for i := 1 to 10 do
    if flags[i] then
      count := count + 1;
  writeln(count);

  { Toggle all flags }
  for i := 1 to 10 do
    if flags[i] then
      flags[i] := false
    else
      flags[i] := true;

  { Count true flags again }
  count := 0;
  for i := 1 to 10 do
    if flags[i] then
      count := count + 1;
  writeln(count);

  { Print first 6 as 0/1 }
  for i := 1 to 6 do begin
    if flags[i] then
      write('1')
    else
      write('0');
  end;
  writeln;
end.
