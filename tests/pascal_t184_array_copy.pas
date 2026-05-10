{ Test array element-wise copy }
program TestArrayCopy;
type
  IntArr = array[1..5] of integer;
var
  src, dst: IntArr;
  i: integer;
begin
  for i := 1 to 5 do
    src[i] := i * 100;

  { copy element by element }
  for i := 1 to 5 do
    dst[i] := src[i];

  { verify independence }
  src[1] := 999;

  for i := 1 to 5 do begin
    if i > 1 then write(' ');
    write(dst[i]);
  end;
  writeln;
  writeln(src[1]);
end.
