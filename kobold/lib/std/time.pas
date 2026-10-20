unit std.time;
{ Monotonic time and cooperative scheduling as a unit, over the host clock and
  the park/yield seam (runtime/rng_glue.c kp_now_ms, runtime/sched_glue.c
  kp_sleep_ms).  In the std tier because these need a host clock and scheduler; a
  bare-metal target has neither.  A guest that waits should Sleep rather than
  spin; a bare give-up-the-turn yield is Sleep(0) until coroutine scheduling
  lands as its own namespaced unit. }
interface

function Now: Int64;                  { monotonic milliseconds since an epoch }
procedure Sleep(ms: Integer);         { park the task for a duration }

implementation

function kp_now_ms: Int64; external cdecl;
procedure kp_sleep_ms(ms: Integer); external cdecl;

function Now: Int64;
begin
  Now := kp_now_ms;
end;

procedure Sleep(ms: Integer);
begin
  kp_sleep_ms(ms);
end;

end.
