{ Typed files (file of T): write and read whole records, filesize/filepos,
  random access with seek, and the ISO file buffer variable f^ with get/put
  for an in-place update.  The temp file lives under build/ and is erased at
  the end, so the test leaves no artifact. }
program FileTyped;
type
  TRec = record
    id: integer;
    val: integer;
  end;
var
  f: file of TRec;
  r: TRec;
  i: integer;
begin
  assign(f, 'build/pc_file_t244.tmp');
  rewrite(f);
  for i := 1 to 5 do
  begin
    r.id := i;
    r.val := i * 100;
    write(f, r)
  end;
  close(f);

  reset(f);
  writeln('size=', filesize(f));
  while not eof(f) do
  begin
    read(f, r);
    writeln(r.id, ' ', r.val)
  end;

  { random access: seek to component 2, read it through f^, then get the next }
  seek(f, 2);
  writeln('pos=', filepos(f));
  writeln('fbuf=', f^.id, ' ', f^.val);
  get(f);
  writeln('next pos=', filepos(f), ' fbuf=', f^.id);
  close(f);

  { update component 0 in place through f^ and put }
  reset(f);
  seek(f, 0);
  f^.val := 999;
  put(f);
  close(f);

  reset(f);
  read(f, r);
  writeln('updated=', r.id, ' ', r.val);
  close(f);

  erase(f);
  writeln('done')
end.
