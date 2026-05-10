{ Test manual string trimming }
program TestStringTrim;
var
  s, t: string[30];
  i, start, finish, len: integer;
begin
  s := '   HELLO   ';

  { Find first non-space }
  start := 1;
  while (start <= length(s)) and then (s[start] = ' ') do
    start := start + 1;

  { Find last non-space }
  finish := length(s);
  while (finish >= 1) and then (s[finish] = ' ') do
    finish := finish - 1;

  { Extract trimmed substring }
  t := '';
  len := 0;
  for i := start to finish do begin
    len := len + 1;
    t[len] := s[i];
  end;
  t[0] := chr(len);

  write('[');
  write(t);
  writeln(']');
  writeln(length(t));
end.
