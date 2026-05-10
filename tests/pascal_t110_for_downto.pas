{ Test for..downto loop }
program TestForDownto;
var
  i, sum: integer;
begin
  { count down from 5 to 1 }
  for i := 5 downto 1 do begin
    if i < 5 then write(' ');
    write(i);
  end;
  writeln;

  { sum 10 downto 1 = 55 }
  sum := 0;
  for i := 10 downto 1 do
    sum := sum + i;
  writeln(sum);

  { single iteration }
  for i := 3 downto 3 do
    write(i);
  writeln;

  { zero iterations: start < end }
  sum := 99;
  for i := 1 downto 5 do
    sum := 0;
  writeln(sum);
end.
