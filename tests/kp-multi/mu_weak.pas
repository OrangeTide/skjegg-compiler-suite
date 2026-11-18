program mu_weak;
{ The weak field of an imported record type is non-retaining across the unit
  boundary: holding a process only through h.w does not keep it alive, so dropping
  the sole strong reference reclaims the coroutine and the field reads back nil.
  If the round-trip dropped the weak flag the field would retain, and the second
  line would read 'after:alive'. }
uses wkdep as wk;

procedure body;
begin
  yield 1;
end;

var
  h: wk.THolder;
  p: process;
begin
  p := start body;
  h.w := p;
  if h.w = nil then writeln('before:nil') else writeln('before:alive');
  p := nil;
  if h.w = nil then writeln('after:nil') else writeln('after:alive');
end.
