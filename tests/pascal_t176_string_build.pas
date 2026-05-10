{ Test building strings with concat }
program TestStringBuild;
var
  s, t: string[30];
begin
  s := 'AB';
  t := 'CD';
  s := concat(s, t);
  writeln(s);
  writeln(length(s));

  s := concat('X', 'Y', 'Z');
  writeln(s);

  { repeated concat }
  s := '';
  s := concat(s, 'Hello');
  s := concat(s, ' ');
  s := concat(s, 'World');
  writeln(s);
end.
