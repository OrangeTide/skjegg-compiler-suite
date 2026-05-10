program WriteHalt;
{ Test: write output and then halt with non-zero exit code.
  This exercises both fd_write and proc_exit imports together. }
var x: integer;
begin
  x := 7;
  writeln(x);
  halt(1)
end.
