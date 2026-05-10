{ Test combined string operations }
program TestStringOps;
var
  s, t, u: string[40];
  i, code: integer;
begin
  s := 'Hello';
  t := ' World';
  u := concat(s, t);
  writeln(u);
  writeln(length(u));

  { copy }
  s := copy(u, 7, 5);
  writeln(s);

  { pos }
  writeln(pos('World', u));

  { delete }
  s := 'ABCDEF';
  delete(s, 3, 2);
  writeln(s);

  { insert }
  s := 'ABEF';
  insert('CD', s, 3);
  writeln(s);

  { val }
  val('12345', i, code);
  writeln(i);
  writeln(code);
end.
