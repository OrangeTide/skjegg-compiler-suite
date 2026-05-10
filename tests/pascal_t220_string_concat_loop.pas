{ Test building strings in a loop with concat }
program TestStringConcatLoop;
var
  s, t: string[30];
  i: integer;
begin
  s := '';
  for i := 1 to 5 do begin
    t := concat(s, '*');
    s := t;
    writeln(s);
  end;
  writeln(length(s));
end.
