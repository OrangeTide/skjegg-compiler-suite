{ Test str built-in }
program TestStr;
var
  s: string[20];
  n: integer;
begin
  n := 42;
  str(n, s);
  writeln(s);
  writeln(length(s));

  n := -999;
  str(n, s);
  writeln(s);

  n := 0;
  str(n, s);
  writeln(s);

  n := 1000000;
  str(n, s);
  writeln(s);
end.
