program t031_string_length;
var
  s: string;
  t: string[10];
  a: string;
begin
  s := 'Hello';
  writeln(length(s));
  t := 'World!';
  writeln(length(t));
  s := '';
  writeln(length(s));
  writeln(length('test'));

  { string-to-string assignment }
  s := 'source';
  a := s;
  writeln(a);
  writeln(length(a));

  { truncation on string-to-string }
  s := 'This is long text';
  t := s;
  writeln(t);
  writeln(length(t))
end.
