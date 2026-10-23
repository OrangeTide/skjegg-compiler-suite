{ Test building repeated strings by ansistring concatenation }
program TestStringRepeat;
var
  s: string;
  d: array[1..3] of string;
  i: integer;
begin
  d[1] := '1';
  d[2] := '2';
  d[3] := '3';

  { Build "AAAA" }
  s := '';
  for i := 1 to 4 do
    s := s + 'A';
  writeln(s);

  { Build "12321" }
  s := '';
  for i := 1 to 3 do
    s := s + d[i];
  for i := 2 downto 1 do
    s := s + d[i];
  writeln(s);

  writeln(length(s));
end.
