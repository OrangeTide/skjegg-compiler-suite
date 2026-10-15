program conf_trap_divzero;
{ Conformance: integer div by a zero divisor is an always-on DIV_ZERO trap that
  exits 70, on every Kobold tier, JIT and AOT (kp-evaluation.md, kp-abi.md). The
  divisor is a variable so the guard is exercised; a literal zero traps the same
  way. The trap fires before emit runs, so stdout is empty and the fixture pins
  only the exit code. }
procedure emit(v: integer); external;

var
  a, b: integer;
begin
  a := 1;
  b := 0;
  emit(a div b);
end.
