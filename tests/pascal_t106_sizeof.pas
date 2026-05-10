{ Test sizeof builtin }
program TestSizeof;
type
  Point = record
    x, y: integer;
  end;
  Color = record
    r, g, b, a: byte;
  end;
var
  n: integer;
begin
  writeln(sizeof(Point));
  writeln(sizeof(Color));

  n := sizeof(Point) + sizeof(Color);
  writeln(n);
end.
