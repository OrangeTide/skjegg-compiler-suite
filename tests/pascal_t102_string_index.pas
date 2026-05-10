{ Test string indexing - read and write individual characters }
program TestStringIndex;
var
  s: string[20];
  i: integer;
  ch: char;
begin
  s := 'Hello';

  { read characters }
  for i := 1 to length(s) do
    write(s[i]);
  writeln;

  { write characters }
  s[1] := 'h';
  s[5] := 'O';
  writeln(s);

  { swap first and last }
  ch := s[1];
  s[1] := s[5];
  s[5] := ch;
  writeln(s);

  { index from expression }
  i := 3;
  writeln(s[i]);
end.
