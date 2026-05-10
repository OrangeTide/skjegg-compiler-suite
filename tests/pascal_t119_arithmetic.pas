{ Test arithmetic operations and edge cases }
program TestArithmetic;
var
  a, b: integer;
begin
  { basic ops }
  a := 17;
  b := 5;
  writeln(a + b);
  writeln(a - b);
  writeln(a * b);
  writeln(a div b);
  writeln(a mod b);

  { negative operands }
  writeln(-17 div 5);
  writeln(-17 mod 5);

  { large multiply }
  a := 12345;
  b := 6789;
  writeln(a * b);
end.
