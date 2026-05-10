{ Test deep recursion (Ackermann) }
program TestDeepRecursion;

function ack(m, n: integer): integer;
begin
  if m = 0 then
    ack := n + 1
  else if n = 0 then
    ack := ack(m - 1, 1)
  else
    ack := ack(m - 1, ack(m, n - 1));
end;

begin
  writeln(ack(0, 0));
  writeln(ack(1, 1));
  writeln(ack(2, 2));
  writeln(ack(3, 3));
  writeln(ack(3, 4));
end.
