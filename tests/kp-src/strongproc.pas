program strongproc;
{ The negative control for weakproc: an ordinary (strong) process field retains
  its referent, so dropping the other strong reference does not reclaim the
  coroutine and the field still reads alive. This is the leak a weak field
  avoids: swap 'w' to 'weak w' (as weakproc does) and 'after' reads nil. }
type
  THolder = record
    w: process;
  end;

procedure body;
begin
  yield 1;
end;

var
  h: THolder;
  p: process;
begin
  p := start body;
  h.w := p;
  if h.w = nil then writeln('before:nil') else writeln('before:alive');
  p := nil;
  if h.w = nil then writeln('after:nil') else writeln('after:alive');
end.
