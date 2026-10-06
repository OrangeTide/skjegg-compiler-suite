program failmethod;

{ A failable method, both paths, run end to end (exercises the CALL2 emission in the method-call
  lowering). bump mutates its record receiver and returns the new value, or fails on a zero step. }

type Counter = record value: integer; end;

function bump for (c: Counter)(by: integer): integer fails;
begin
  if by = 0 then fail(22);
  c.value := c.value + by;
  bump := c.value;
end;

var
  c: Counter;

begin
  c.value := 10;
  writeln(c.bump(3) or else -1);          { 13 }
  writeln(c.bump(4) or else -1);          { 17: the receiver mutation persisted }
  if c.bump(0) fails e then writeln(errorcode(e)) else writeln(0);   { 22 }
  writeln(c.value);                        { 17: the failed bump did not mutate }
end.
