program factorial;
var n, f: integer;
begin
  n := 10;
  f := 1;
  while n > 1 do
  begin
    f := f * n;
    n := n - 1
  end;
  halt(f mod 256)
end.
