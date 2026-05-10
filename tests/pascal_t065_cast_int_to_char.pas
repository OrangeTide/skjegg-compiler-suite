program CastIntToChar;
var c: char;
begin
  c := char(72);
  writeln(ord(c));
  c := char(300);
  writeln(ord(c));
  writeln(integer(char(300)))
end.
