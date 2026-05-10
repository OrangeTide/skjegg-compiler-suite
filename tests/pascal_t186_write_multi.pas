{ Test write with multiple arguments }
program TestWriteMulti;
var
  x: integer;
begin
  write('A', 'B', 'C');
  writeln;

  writeln('x=', 42);

  x := 100;
  writeln('value: ', x);

  write(1, ' ', 2, ' ', 3);
  writeln;
end.
