{ ParamStr/ParamCount command-line access (P6 stage D).  main captures argc and
  argv; ParamCount is the argument count after the program name and ParamStr(i)
  returns argument i (an out-of-range index is the empty string).  The gate runs
  with no arguments, so ParamCount is 0 and the loop body does not run.  Host
  only: the freestanding VM backends receive no command line. }
program Params;
var
  i: integer;
begin
  writeln('count=', ParamCount);
  for i := 1 to ParamCount do
    writeln(i, ': ', ParamStr(i));
  writeln('oob=', Length(ParamStr(99)));
end.
