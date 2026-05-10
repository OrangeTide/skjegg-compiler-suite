program t040_array_2d;
type
  TMatrix = array[1..3, 1..3] of integer;
var
  m: TMatrix;
  r, c: integer;
begin
  for r := 1 to 3 do
    for c := 1 to 3 do
      m[r, c] := r * 10 + c;
  for r := 1 to 3 do begin
    write(m[r, 1]);
    for c := 2 to 3 do
      write(' ', m[r, c]);
    writeln;
  end;
end.
