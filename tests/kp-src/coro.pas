program coro;

procedure counter;
begin
  yield 1;
  yield 2;
  yield 3;
end;

var
  k: process;
begin
  k := start counter;
  WriteLn(k(0));
  WriteLn(k(0));
  WriteLn(k(0));
end.
