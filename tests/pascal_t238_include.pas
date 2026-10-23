{ Include-file splicing in expression position (P6 stage C): the dollar-I
  directive splices an included file's text where it appears, resuming the
  parent source at its end.  Here each include is a constant's value, a string
  literal and an integer, resolved relative to this source file's directory. }
program IncludeConst;
const
  VER = {$I t238_version.inc};
  N = {$I t238_count.inc};
begin
  writeln(VER);
  writeln(N * 2);
end.
