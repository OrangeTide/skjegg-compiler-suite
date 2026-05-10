program t033_readln_string;
var
  s: string;
  t: string[5];
begin
  readln(s);
  writeln(s);
  writeln(length(s));

  { truncation to string[5] }
  readln(t);
  writeln(t);
  writeln(length(t));

  { empty line }
  readln(s);
  writeln(length(s));
end.
