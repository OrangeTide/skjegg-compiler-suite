program conf_trap_assert;
{ Conformance: assert(cond) traps and exits 70 when cond is false.  stdout is
  empty because the trap fires before any output (kp-abi.md runtime faults).
  The value is in a variable so the check runs at runtime. }
procedure emit(v: integer); external;

var
  x: integer;
begin
  x := 1;
  assert(x = 0);
  emit(x);
end.
