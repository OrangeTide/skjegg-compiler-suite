{ read parses a real from the input stream: a plain decimal, scientific form,
  and a single, with an integer read still working afterward (the reader stops
  at and pushes back the first non-number byte). }
program ReadReal;
var
  a, b: real;
  s: single;
  i: integer;
begin
  read(a);
  read(b);
  read(s);
  read(i);
  writeln(a:0:2);
  writeln(b:0:3);
  writeln(s:0:1);
  writeln(i)
end.
