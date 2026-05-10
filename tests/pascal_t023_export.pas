program t023_export;

{$EXPORT add_numbers}
function AddNumbers(a: integer; b: integer): integer;
begin
  AddNumbers := a + b;
end;

begin
  writeln(AddNumbers(17, 25));
end.
