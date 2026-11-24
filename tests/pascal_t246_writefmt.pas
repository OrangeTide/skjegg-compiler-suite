{ Write field width for integers, strings and chars: write(x:w) right-justifies
  in a field w wide (a minimum; a wider value prints in full). }
program WriteFmt;
var
  i: integer;
  s: string;
  c: char;
  big: int64;
begin
  i := 42;
  s := 'hi';
  c := 'X';
  big := 1000000000000;
  writeln(i:5, '|');
  writeln(-7:4, '|');
  writeln(s:5, '|');
  writeln(c:3, '|');
  writeln(big:16, '|');
  writeln(i:1, '|');
  writeln('ab':1, '|')
end.
