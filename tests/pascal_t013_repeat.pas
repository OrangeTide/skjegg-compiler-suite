program repeattest;
var n: integer;
begin
  n := 1;
  repeat
    write(n);
    writeln(' hello');
    n := n + 1
  until n > 3
end.
