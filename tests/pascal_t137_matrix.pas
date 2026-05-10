{ Test 2D array (matrix) operations }
program TestMatrix;
type
  Matrix = array[1..3] of array[1..3] of integer;
var
  a, b: Matrix;
  i, j: integer;
begin
  { identity matrix }
  for i := 1 to 3 do
    for j := 1 to 3 do
      if i = j then
        a[i][j] := 1
      else
        a[i][j] := 0;

  { print }
  for i := 1 to 3 do begin
    for j := 1 to 3 do begin
      if j > 1 then write(' ');
      write(a[i][j]);
    end;
    writeln;
  end;

  { fill with multiplication table }
  for i := 1 to 3 do
    for j := 1 to 3 do
      b[i][j] := i * j;

  for i := 1 to 3 do begin
    for j := 1 to 3 do begin
      if j > 1 then write(' ');
      write(b[i][j]);
    end;
    writeln;
  end;
end.
