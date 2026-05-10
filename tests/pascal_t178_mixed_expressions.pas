{ Test mixed arithmetic expressions }
program TestMixedExpr;
var
  a, b, c: integer;
begin
  a := 10; b := 3; c := 7;
  writeln(a + b * c);
  writeln((a + b) * c);
  writeln(a * b + c);
  writeln(a * (b + c));

  { operator precedence }
  writeln(2 + 3 * 4 - 1);
  writeln(100 div 10 mod 3);
  writeln(100 - 50 - 25);

  { nested parens }
  writeln(((a + b) * (c - 1)) div 2);
end.
