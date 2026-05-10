{ Test nested procedures }
program TestNestedProc;
var
  total: integer;

procedure outer(n: integer);
var
  subtotal: integer;

  procedure inner(x: integer);
  begin
    subtotal := subtotal + x;
    total := total + x;
  end;

var
  i: integer;
begin
  subtotal := 0;
  for i := 1 to n do
    inner(i);
  writeln(subtotal);
end;

begin
  total := 0;
  outer(5);
  outer(3);
  writeln(total);
end.
