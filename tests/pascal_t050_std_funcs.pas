program t050_std_funcs;

var
  i: integer;

begin
  { ord }
  writeln(ord(true));
  writeln(ord(false));

  { abs }
  writeln(abs(-5));
  writeln(abs(5));
  writeln(abs(0));

  { odd }
  if odd(3) then write('yes') else write('no');
  write(' ');
  if odd(4) then write('yes') else write('no');
  writeln;

  { succ / pred }
  writeln(succ(5));
  writeln(pred(5));
  writeln(succ(0));
  writeln(pred(0));

  { sqr }
  writeln(sqr(5));
  writeln(sqr(-3));
  writeln(sqr(0));

  { lo / hi }
  i := $1234;
  writeln(lo(i));
  writeln(hi(i));
  writeln(lo(255));
  writeln(hi(255));
  writeln(lo(256));
  writeln(hi(256))
end.
