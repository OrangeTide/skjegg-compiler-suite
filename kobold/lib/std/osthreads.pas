unit std.osthreads;
{ Real OS threads as a native-backed unit (doc/languages/kobold/kp-native-units.md),
  the preemptive counterpart to the cooperative std.threads. A thread body is a
  Kobold routine handed to the C glue (osthreads_glue.c) and run on a platform
  thread; this is possible because a Kobold procedure value is a C function pointer.

  Shared-nothing is the contract, not a style: only scalars cross the boundary. A
  worker takes one integer and returns one integer, and MUST NOT touch a string,
  array, map, or any other managed value shared with another thread. The runtime's
  reference counting and heap are single-threaded, so sharing a managed object
  across threads is undefined. Within that rule this gives real parallelism for
  compute kernels: spawn several workers, let them run on separate cores, join each
  for its result.

  Native-backed, so ahead-of-time only (the JIT cannot bind external symbols). }
interface

type
  { A thread body: one integer payload in, one integer result out. }
  Worker = function(arg: integer): integer cdecl;

{ Start body(arg) on a real OS thread. Returns a thread handle (an integer id), or
  -1 on failure. Take the body with `@`, since a bare parameterless worker name would
  be a call: Spawn(@work, n). }
function Spawn(body: Worker; arg: integer): integer;

{ Wait for a thread to finish and return its worker's result; reclaims the handle.
  Returns -1 for an invalid handle. }
function Join(t: integer): integer;

{ The number of hardware threads available (at least 1). }
function HardwareThreads: integer;

implementation

{ The pthread glue, declared once here; resolved at link from the object archived
  in this unit, with -lpthread from the unit's .link sidecar. }
function kp_osthread_spawn(body: Worker; arg: integer): integer; cdecl; external;
function kp_osthread_join(id: integer): integer; cdecl; external;
function kp_osthread_hwconc: integer; cdecl; external;

function Spawn(body: Worker; arg: integer): integer;
begin
  Spawn := kp_osthread_spawn(body, arg);
end;

function Join(t: integer): integer;
begin
  Join := kp_osthread_join(t);
end;

function HardwareThreads: integer;
begin
  HardwareThreads := kp_osthread_hwconc;
end;

end.
