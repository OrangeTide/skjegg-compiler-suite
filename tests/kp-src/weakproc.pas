program weakproc;
{ A weak process field does not keep its referent alive: once the only strong
  reference is dropped the coroutine is reclaimed and the weak field reads nil. }
type
  THolder = record
    weak w: process;
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
