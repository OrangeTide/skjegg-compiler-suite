program t081_str;
var
  s: string[20];
begin
  str(42, s);
  writeln(s);
  str(-123, s);
  writeln(s);
  str(0, s);
  writeln(s);
  str(2147483647, s);
  writeln(s);
  str(-1, s);
  writeln(s)
end.
