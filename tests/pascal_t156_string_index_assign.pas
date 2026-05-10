{ Test string indexing and assignment }
program TestStringIndex;
var
  s: string[20];
  i: integer;
begin
  s := 'Hello World';
  writeln(s[1]);
  writeln(s[6]);
  writeln(s[11]);

  { modify characters }
  s[1] := 'h';
  s[7] := 'w';
  writeln(s);

  { build string char by char }
  s := 'ABCDE';
  for i := 1 to 5 do
    s[i] := chr(ord(s[i]) + 32);
  writeln(s);
end.
