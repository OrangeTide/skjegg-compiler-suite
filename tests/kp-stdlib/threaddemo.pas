program threaddemo;
{ Exercises std.threads over the freestanding rv tier: a run queue of cooperative
  tasks driven by RunAll, a shared counter guarded by a Mutex, and a Condition a
  task waits on until another task signals it.  The schedule is a deterministic
  round-robin, so the output is stable. }
uses std.threads as _;

var
  total: Integer;
  m: Mutex;
  c: Condition;

procedure counter(id, n: Integer);
var
  i: Integer;
begin
  for i := 1 to n do
  begin
    Lock(m);
    total := total + 1;
    WriteLn(total);
    Unlock(m);
    Pause;
  end;
  Signal(c);          { let the waiter proceed once the count is in }
  Finish;
end;

procedure waiter;
begin
  Wait(c);
  WriteLn(100);
  Finish;
end;

begin
  total := 0;
  m.held := false;
  c.gen := 0;
  Ready(start counter(1, 3));
  Ready(start waiter);
  RunAll;
  WriteLn(999);
end.
