program cofield;
{ A coroutine held in a record field and resumed through the field designator. Prints
  51 then 52. }
procedure emit(v: integer); external;

procedure worker(id: integer);
begin
  yield id * 10 + 1;
  yield id * 10 + 2;
end;

type
  Slot = record k: process; end;

var s: Slot;
begin
  s.k := start worker(5);
  emit(s.k(0));
  emit(s.k(0));
  release(s.k);
end.
