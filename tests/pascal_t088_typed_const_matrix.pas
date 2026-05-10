program TypedConstMatrix;
const
  m: array[1..2, 1..3] of integer = ((1, 2, 3), (4, 5, 6));
var
  i, j: integer;
begin
  for i := 1 to 2 do begin
    for j := 1 to 3 do begin
      write(m[i, j]);
      write(' ')
    end;
    writeln
  end
end.
