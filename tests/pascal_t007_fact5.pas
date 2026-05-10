program fact5;
var n, f: integer;
begin
  n := 5;
  f := 1;
  while n > 1 do
  begin
    f := f * n;
    n := n - 1
  end;
  halt(f)
end.
