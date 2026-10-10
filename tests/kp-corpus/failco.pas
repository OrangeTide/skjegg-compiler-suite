program failco;

{ A coroutine is not failable, but its body may call a failable routine and handle it locally
  (doc/languages/kobold/kp-errors.md). Here the worker yields each parsed value, recovering a negative input with
  `or else`, so the failable call-site form composes inside a suspended frame. }

function parse(n: integer): integer fails;
begin
  if n < 0 then fail(7);
  parse := n * 10;
end;

procedure worker;
var
  i: integer;
begin
  for i := 2 downto -1 do
    yield parse(i) or else -1;
end;

var
  k: process;
  n: integer;

begin
  k := start worker;
  for n := 0 to 3 do
    writeln(k(0));
  release(k);
end.
