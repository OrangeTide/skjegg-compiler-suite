{ Test copy function edge cases }
program TestCopyFunc;
var
  s, t: string[20];
begin
  s := 'ABCDEFGH';

  { from start }
  t := copy(s, 1, 3);
  writeln(t);

  { from end }
  t := copy(s, 6, 3);
  writeln(t);

  { single char }
  t := copy(s, 4, 1);
  writeln(t);

  { full string }
  t := copy(s, 1, 8);
  writeln(t);
end.
