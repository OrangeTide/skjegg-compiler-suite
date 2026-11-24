{ Named text files: assign/rewrite/writeln to a file, then reset and read it
  back line by line with eof.  Then IOResult under the I-minus directive when
  opening a missing file, and erase to remove the temp file.  The file lives
  under build/ and is erased at the end, so the test leaves no artifact. }
program FileText;
var
  f: text;
  s: string;
  n: integer;
begin
  assign(f, 'build/pc_file_t242.tmp');
  rewrite(f);
  writeln(f, 'alpha');
  writeln(f, 'beta');
  writeln(f, 'gamma');
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

  { IOResult: opening a missing file with I/O checks off records an error,
    does not trap, and leaves IOResult non-zero (cleared when read). }
  assign(f, 'build/no_such_file_pc_t242');
  {$I-}
  reset(f);
  {$I+}
  writeln('ioresult=', IOResult);
  writeln('ioresult2=', IOResult);   { cleared, so 0 now }

  { remove the temp file }
  assign(f, 'build/pc_file_t242.tmp');
  erase(f);
  writeln('done')
end.
