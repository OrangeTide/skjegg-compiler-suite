{ Test manual string trimming with ansistring copy }
program TestStringTrim;
var
  s, t: string;
  start, finish: integer;
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
  t := copy(s, start, finish - start + 1);

  write('[');
  write(t);
  writeln(']');
  writeln(length(t));
end.
