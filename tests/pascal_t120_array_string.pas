{ Test array of strings }
program TestArrayString;
type
  StrArr = array[1..3] of string[10];
var
  names: StrArr;
  i: integer;
begin
  names[1] := 'Alpha';
  names[2] := 'Beta';
  names[3] := 'Gamma';
  for i := 1 to 3 do
    writeln(names[i]);
  writeln(length(names[2]));
end.
