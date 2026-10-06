program writereal;
{ Real output through write and writeln, which lower to the io_write_real external
  (a stream id and an f64 value). A single is widened to f64 first. Run under kprun
  it prints, one per line: 3.14, half = 2.5, 0.25, 1.5 2.5, 2.5. }
var
  r: real;
  s: single;
begin
  writeln(3.14);
  writeln('half = ', 2.5);
  r := 1.0 / 4.0;
  writeln(r);
  write(1.5); write(' '); writeln(2.5);
  s := 2.5;
  writeln(s);
end.
