{ Test string edge cases }
program TestStringEdge;
var
  s: string[20];
  t: string[5];
begin
  { empty string }
  s := '';
  writeln(length(s));

  { single char }
  s := 'X';
  writeln(length(s));
  writeln(s);

  { full length }
  s := '12345678901234567890';
  writeln(length(s));
  writeln(s);

  { truncation on short string }
  t := 'ABCDEFGHIJ';
  writeln(length(t));
  writeln(t);

  { concatenation }
  s := 'Hello';
  s := concat(s, ' ', 'World');
  writeln(s);

  { copy from middle }
  s := 'ABCDEFGH';
  t := copy(s, 3, 4);
  writeln(t);
end.
