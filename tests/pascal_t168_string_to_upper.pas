{ Test converting string to uppercase }
program TestStringToUpper;
var
  s: string[30];
  i: integer;
begin
  s := 'Hello World 123!';
  for i := 1 to length(s) do
    s[i] := upcase(s[i]);
  writeln(s);

  { lowercase function }
  s := 'HELLO WORLD';
  for i := 1 to length(s) do
    s[i] := lowercase(s[i]);
  writeln(s);
end.
