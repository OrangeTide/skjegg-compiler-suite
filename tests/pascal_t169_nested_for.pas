{ Test nested for loops }
program TestNestedFor;
var
  i, j, count: integer;
begin
  { multiplication table 1..4 }
  for i := 1 to 4 do begin
    for j := 1 to 4 do begin
      if j > 1 then write(' ');
      write(i * j);
    end;
    writeln;
  end;

  { count pairs where i+j=5 }
  count := 0;
  for i := 1 to 4 do
    for j := 1 to 4 do
      if i + j = 5 then
        count := count + 1;
  writeln(count);
end.
