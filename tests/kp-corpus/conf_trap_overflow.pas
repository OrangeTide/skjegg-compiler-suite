program conf_trap_overflow;
{$Q+}
{ Conformance: with overflow checking enabled ($Q+), a signed 32-bit add that
  overflows traps OVERFLOW and exits 70; overflow checking is off by default,
  where the operation wraps (see conf_wrap) (kp-abi.md, kp-evaluation.md). The
  value is in a variable so the checked add runs at runtime. The trap fires
  before emit runs, so stdout is empty. }
procedure emit(v: integer); external;

var
  a: integer;
begin
  a := 2147483647;
  a := a + 1;
  emit(a);
end.
