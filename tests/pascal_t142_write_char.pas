{ Test writing individual characters }
program TestWriteChar;
var
  i: integer;
begin
  { ASCII box }
  write('+');
  for i := 1 to 8 do write('-');
  writeln('+');

  write('|');
  write(' Hello  ');
  writeln('|');

  write('+');
  for i := 1 to 8 do write('-');
  writeln('+');
end.
