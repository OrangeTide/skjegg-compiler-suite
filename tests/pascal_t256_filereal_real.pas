{ read a real from a named text file with read(f, r), and round-trip reals
  through a typed file of real.  The text read parses a decimal and a
  scientific form on one line; the typed read copies the binary components
  back.  The temp files live under build/ and are erased at the end, so the
  test leaves no artifact. }
program FileReal;
var
  t: text;
  f: file of real;
  a, b, r: real;
begin
  assign(t, 'build/pc_file_t256.tmp');
  rewrite(t);
  writeln(t, '3.5 6.25e-1');
  close(t);
  reset(t);
  read(t, a);
  read(t, b);
  close(t);
  erase(t);
  writeln(a:0:2);
  writeln(b:0:3);

  assign(f, 'build/pc_file_t256b.tmp');
  rewrite(f);
  r := 1.25;
  write(f, r);
  r := -2.5;
  write(f, r);
  close(f);
  reset(f);
  read(f, r);
  writeln(r:0:2);
  read(f, r);
  writeln(r:0:2);
  close(f);
  erase(f);
  writeln('done')
end.
