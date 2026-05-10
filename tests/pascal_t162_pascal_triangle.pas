{ Test Pascal's triangle }
program TestPascalTriangle;
var
  row: array[0..6] of integer;
  i, j: integer;
begin
  for i := 0 to 6 do begin
    row[i] := 1;
    for j := i - 1 downto 1 do
      row[j] := row[j] + row[j - 1];
    for j := 0 to i do begin
      if j > 0 then write(' ');
      write(row[j]);
    end;
    writeln;
  end;
end.
