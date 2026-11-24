{ Untyped files: fixed-size block transfer with blockwrite/blockread, the
  optional transferred-count result, and rename.  An array of four integers is
  written as four records, read back, and verified; then the file is renamed
  and erased so the test leaves no artifact. }
program FileUntyped;
type
  TBuf = array[1..4] of integer;
var
  f: file;
  a, b: TBuf;
  i, nw, nr: integer;
begin
  a[1] := 10;
  a[2] := 20;
  a[3] := 30;
  a[4] := 40;

  assign(f, 'build/pc_file_t243.tmp');
  rewrite(f, 4)        { record size = 4-byte integer };
  blockwrite(f, a, 4, nw);
  close(f);
  writeln('wrote ', nw);

  reset(f, 4);
  blockread(f, b, 4, nr);
  close(f);
  writeln('read ', nr);
  for i := 1 to 4 do
    writeln(b[i]);

  { rename updates the file's own name, so erase then removes the renamed file }
  rename(f, 'build/pc_file_t243b.tmp');
  erase(f);
  writeln('done')
end.
