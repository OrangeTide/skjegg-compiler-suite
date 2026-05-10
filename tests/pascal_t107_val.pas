{ Test val builtin - string to integer conversion }
program TestVal;
var
  s: string[20];
  v, code: integer;
begin
  s := '42';
  val(s, v, code);
  writeln(v);
  writeln(code);

  s := '-7';
  val(s, v, code);
  writeln(v);
  writeln(code);

  s := '  123';
  val(s, v, code);
  writeln(v);
  writeln(code);

  s := '12x5';
  val(s, v, code);
  writeln(code);

  s := 'abc';
  val(s, v, code);
  writeln(code);
end.
