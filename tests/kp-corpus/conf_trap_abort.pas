program conf_trap_abort;
{ Conformance: abort is an unconditional fatal (exit 70).  stdout is empty
  because abort fires before any output (kp-abi.md runtime faults). }
procedure emit(v: integer); external;

begin
  abort;
  emit(1);
end.
