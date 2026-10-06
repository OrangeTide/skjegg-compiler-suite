program scheduler;
{ A cooperative scheduler that stores several coroutines and, on each tick, chooses which
  one to resume by a runtime selector. Every worker owns a nested coroutine handle, so
  dropping a worker mid-flight must release that handle as its frame unwinds. Run under
  kprun the round-robin schedule prints 11 21 31 12 22 32. }
procedure emit(v: integer); external;

{ a nested coroutine the worker owns and would release on normal completion; a worker
  dropped mid-flight must release it through the cancel-unwind cleanup instead }
procedure leaf;
begin
  yield 0;
end;

procedure worker(id, steps: integer);
var sub: process; i: integer;
begin
  sub := start leaf;
  sub(0);
  for i := 1 to steps do
    yield id * 10 + i;
  release(sub);
end;

var
  a, b, c: process;
  n, turn: integer;
begin
  a := start worker(1, 5);   { store several coroutines }
  b := start worker(2, 5);
  c := start worker(3, 5);
  for n := 0 to 5 do          { resume across ticks, choosing one each time }
  begin
    turn := n mod 3;
    if turn = 0 then emit(a(0))
    else if turn = 1 then emit(b(0))
    else emit(c(0));
  end;
  release(a);                 { drop all three mid-flight; each releases its owned leaf }
  release(b);
  release(c);
end.
