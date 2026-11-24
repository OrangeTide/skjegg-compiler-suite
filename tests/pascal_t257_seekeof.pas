{ SeekEof skips blanks and line ends before testing end of input; SeekEoln
  skips blanks and tabs before testing end of line.  Read three integers that
  are separated and trailed by spaces and blank lines: SeekEof stays false
  while numbers remain and turns true once only whitespace is left, and
  SeekEoln reports the end of each line past the trailing spaces. }
program SeekTest;
var
  n, count, sum: integer;
begin
  count := 0;
  sum := 0;
  while not seekeof do
  begin
    read(n);
    count := count + 1;
    sum := sum + n;
    if seekeoln then
      writeln('n=', n, ' eoln')
    else
      writeln('n=', n)
  end;
  writeln('count=', count, ' sum=', sum)
end.
