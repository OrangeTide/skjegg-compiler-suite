{ Test sqr and abs }
program TestSqrAbs;
var
  i: integer;
begin
  writeln(sqr(0));
  writeln(sqr(1));
  writeln(sqr(7));
  writeln(sqr(-5));
  writeln(sqr(100));

  writeln(abs(0));
  writeln(abs(42));
  writeln(abs(-42));
  writeln(abs(-1));

  { combined }
  for i := -3 to 3 do
    write(sqr(abs(i)));
  writeln;
end.
