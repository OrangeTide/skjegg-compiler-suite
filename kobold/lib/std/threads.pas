unit std.threads;
{ Cooperative threads as a unit, the Kobold parallel to the C <threads.h> subset
  (include/threads.h, libc/threads.c).  Both tiers ride the same cooperative
  model over the shared stackful-coroutine primitive: one real thread, no
  preemption, a task runs until it yields (Pause) or finishes (Finish).  Here the
  coroutine primitive is the language's own `start` / `yield` / `process` /
  `release`, so this unit is pure Kobold and needs no runtime glue.

  A task is a routine the caller `start`s and hands to the scheduler: Ready
  enqueues a suspended coroutine (Ready(start worker(a, b))), and RunAll then
  drives every queued task to completion.

  Inside a task, Pause gives up the turn (the C thrd_yield), and the task MUST
  end by calling Finish (the C thread-return / thrd_exit): a Kobold coroutine
  cannot report completion on its own, and resuming one that has run off its end
  traps, so Finish hands the scheduler the stable "done" marker it stops on.  A
  task that returns without Finish is a usage error.

  Mutex and Condition are the cooperative forms: with no preemption a critical
  section is uncontended unless it spans a Pause, so a lock simply spins on Pause
  until free, and a wait Pauses until the next Signal.  Fairness is the
  scheduler's round-robin. }
interface

type
  { A cooperative mutex.  Initialize with held := false before use. }
  Mutex = record
    held: Boolean;
  end;

  { A cooperative condition variable.  Initialize with gen := 0 before use.
    Signal wakes every current waiter (a broadcast); a plain single-wake form is
    not meaningful without preemption. }
  Condition = record
    gen: Integer;
  end;

procedure Ready(task: process);   { enqueue a `start`ed coroutine on the run queue }
procedure RunAll;                 { round-robin every queued task until all Finish }
procedure Pause;                  { a task gives up the turn (cooperative yield) }
procedure Finish;                 { a task's final act: report completion to RunAll }

procedure Lock(var m: Mutex);     { acquire, Pausing until the mutex is free }
procedure Unlock(var m: Mutex);   { release }
procedure Wait(var c: Condition); { Pause until the next Signal on c }
procedure Signal(var c: Condition); { wake every task waiting on c }

implementation

{ The scheduler resume protocol: a task yields RUNNING at each Pause and yields
  FINISHED once from Finish.  RUNNING is 0 so a bare `yield` (none here) would
  read as "still running"; FINISHED is a value no Pause produces. }
const
  RUNNING  = 0;
  FINISHED = -1;

var
  queue: array of process;     { the run queue, in enqueue order }

procedure Ready(task: process);
begin
  SetLength(queue, Length(queue) + 1);
  queue[High(queue)] := task;
end;

procedure Pause;
begin
  yield RUNNING;
end;

procedure Finish;
begin
  yield FINISHED;
end;

procedure RunAll;
var
  i, r, remaining: Integer;
  done: array of Boolean;
begin
  SetLength(done, Length(queue));
  remaining := Length(queue);
  while remaining > 0 do
    for i := 0 to High(queue) do
      if not done[i] then
      begin
        r := queue[i](0);                 { resume to the task's next Pause }
        if r = FINISHED then
        begin
          done[i] := true;
          remaining := remaining - 1;
          release(queue[i]);              { cancel-unwinds the suspended task }
        end;
      end;
  SetLength(queue, 0);                     { the queue is spent; ready it again }
end;

procedure Lock(var m: Mutex);
begin
  while m.held do
    Pause;
  m.held := true;
end;

procedure Unlock(var m: Mutex);
begin
  m.held := false;
end;

procedure Wait(var c: Condition);
var
  g: Integer;
begin
  g := c.gen;
  while c.gen = g do
    Pause;
end;

procedure Signal(var c: Condition);
begin
  c.gen := c.gen + 1;
end;

end.
