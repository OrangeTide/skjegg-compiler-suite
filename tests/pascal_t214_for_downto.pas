{ Test for-downto loops }
program TestForDownto;
var
  i, sum: integer;
begin
  { Print countdown }
  for i := 5 downto 1 do
    writeln(i);

  { Sum in reverse }
  sum := 0;
  for i := 10 downto 1 do
    sum := sum + i;
  writeln(sum);

  { Negative range downto }
  for i := 2 downto -2 do
    write(i);
  writeln;
end.
