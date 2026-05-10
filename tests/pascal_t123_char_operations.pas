{ Test character operations }
program TestCharOps;
var
  ch: char;
  i: integer;
begin
  ch := 'A';
  writeln(ch);
  writeln(ord(ch));

  ch := chr(66);
  writeln(ch);

  { succ and pred }
  ch := 'M';
  writeln(succ(ch));
  writeln(pred(ch));

  { char comparison }
  if 'a' < 'z' then
    writeln('a < z');
  if 'Z' < 'a' then
    writeln('Z < a');

  { char in expression }
  i := ord('9') - ord('0');
  writeln(i);

  { upcase }
  ch := 'g';
  writeln(upcase(ch));
end.
