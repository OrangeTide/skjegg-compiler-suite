program weakreuse;
{ Slot reuse with generation carry. The first coroutine is weakly referenced,
  then reclaimed, freeing its registry slot. A second coroutine reuses that slot
  under a bumped generation. The stale weak to the first must still read nil while
  a fresh weak to the second resolves. This holds on every tier: the hosted handle
  table and the freestanding coroutine registry both bump a per-slot generation on
  reclaim and carry it across occupants. }
type
  THolder = record
    weak w: process;
  end;

procedure body;
begin
  yield 1;
end;

var
  h1, h2: THolder;
  p: process;
begin
  p := start body;
  h1.w := p;
  if h1.w = nil then writeln('h1:nil') else writeln('h1:alive');
  p := nil;                 { reclaim the first; its slot is freed, generation bumped }
  if h1.w = nil then writeln('h1after:nil') else writeln('h1after:alive');
  p := start body;          { a second coroutine reuses the freed slot }
  h2.w := p;
  if h1.w = nil then writeln('h1stale:nil') else writeln('h1stale:alive');
  if h2.w = nil then writeln('h2:nil') else writeln('h2:alive');
  p := nil;
end.
