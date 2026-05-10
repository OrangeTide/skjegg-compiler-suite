{ Test inc/dec with optional amount }
program TestIncDecAmount;
var
  x: integer;
begin
  x := 10;
  inc(x);
  writeln(x);

  inc(x, 5);
  writeln(x);

  dec(x);
  writeln(x);

  dec(x, 3);
  writeln(x);

  { inc in loop }
  x := 0;
  while x < 100 do
    inc(x, 7);
  writeln(x);
end.
