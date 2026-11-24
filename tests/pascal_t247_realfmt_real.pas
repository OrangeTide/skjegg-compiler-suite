{ Real write formatting: write(r:w:d) fixed point and write(r:w) scientific. }
program RealFmt;
var r: real;
begin
  r := 3.14159265;
  writeln(r:8:3, '|');
  writeln(r:0:5, '|');
  writeln(r:12, '|');
  writeln(-2.5:8:2, '|')
end.
