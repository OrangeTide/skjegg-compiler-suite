program conf_trap_range;
{$R+}
{ Conformance: with range checking enabled ($R+), an out-of-bounds array index
  traps INDEX_RANGE and exits 70; range checking is off by default, where the
  access is unchecked (kp-abi.md, kp-evaluation.md). The index is a variable so
  the check is a runtime one. The trap fires before emit runs, so stdout is
  empty. }
procedure emit(v: integer); external;

var
  a: array[0..3] of integer;
  i: integer;
begin
  i := 4;
  emit(a[i]);
end.
