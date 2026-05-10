{ Test upcase and lowercase functions }
program TestUpcase;
var
  ch: char;
begin
  writeln(upcase('a'));
  writeln(upcase('z'));
  writeln(upcase('A'));
  writeln(upcase('5'));

  writeln(lowercase('A'));
  writeln(lowercase('Z'));
  writeln(lowercase('a'));
  writeln(lowercase('!'));

  ch := 'm';
  writeln(upcase(ch));
  ch := 'G';
  writeln(lowercase(ch));
end.
