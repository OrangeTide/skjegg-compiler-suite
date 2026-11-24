{ Append opens an existing text file for writing at its end, keeping earlier
  contents (unlike rewrite, which truncates).  Write two lines, close, append a
  third, then read all three back.  The temp file lives under build/ and is
  erased at the end, so the test leaves no artifact. }
program FileAppend;
var
  f: text;
  s: string;
  n: integer;
begin
  assign(f, 'build/pc_file_t252.tmp');
  rewrite(f);
  writeln(f, 'first');
  writeln(f, 'second');
  close(f);

  append(f);
  writeln(f, 'third');
  close(f);

  reset(f);
  n := 0;
  while not eof(f) do
  begin
    readln(f, s);
    n := n + 1;
    writeln(n, ': ', s)
  end;
  close(f);

  erase(f);
  writeln('done')
end.
