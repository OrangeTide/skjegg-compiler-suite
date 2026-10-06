program coteardown;
{ A coroutine left suspended and still referenced at program exit. kp_destroy cancel-
  unwinds it during the teardown handle sweep, which drives its guest code, so that code
  and the runtime thunks must still be mapped then. Prints 11 and must exit cleanly. }
procedure emit(v: integer); external;

procedure worker;
begin
  yield 11;
  yield 22;
end;

var k: process;
begin
  k := start worker;
  emit(k(0));   { runs to the first yield; k stays suspended and is never released }
end.
