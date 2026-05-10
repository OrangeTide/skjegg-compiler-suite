{ Test manual string search (pos-like) }
program TestStringFind;
var
  s: string[30];
  i, found: integer;
begin
  s := 'HELLO WORLD';

  { Find 'W' }
  found := 0;
  for i := 1 to length(s) do
    if (found = 0) and then (s[i] = 'W') then
      found := i;
  writeln(found);

  { Find 'L' - first occurrence }
  found := 0;
  for i := 1 to length(s) do
    if (found = 0) and then (s[i] = 'L') then
      found := i;
  writeln(found);

  { Find 'Z' - not found }
  found := 0;
  for i := 1 to length(s) do
    if (found = 0) and then (s[i] = 'Z') then
      found := i;
  writeln(found);
end.
