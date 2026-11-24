{ Set symmetric difference (a >< b, the FPC operator for elements in exactly
  one of the two sets) and the GetMem/FreeMem untyped heap routines, which take
  an explicit byte size unlike New/Dispose.  Integer only, so it runs on every
  backend. }
program SetMem;
type
  PInt = ^integer;
var
  a, b, c: set of 0..31;
  p: PInt;
  i: integer;
begin
  a := [1, 2, 3, 4, 10];
  b := [3, 4, 5, 6, 10];
  c := a >< b;
  write('symdiff:');
  for i := 0 to 11 do
    if i in c then
      write(' ', i);
  writeln;

  getmem(p, 4);
  p^ := 4321;
  writeln('getmem=', p^);
  freemem(p);
  writeln('done')
end.
