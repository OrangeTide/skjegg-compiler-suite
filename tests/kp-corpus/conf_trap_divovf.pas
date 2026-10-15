program conf_trap_divovf;
{ Conformance: the most negative integer divided by -1 overflows. Its true
  quotient, 2147483648, is not a representable positive int32, so it traps
  OVERFLOW and exits 70, independent of the $Q+ switch (unlike add/sub/mul,
  this division overflow is always checked). The operands are variables so the
  divide runs at runtime; the trap fires before emit, leaving stdout empty. }
procedure emit(v: integer); external;

var
  a, b: integer;
begin
  a := -2147483647 - 1;
  b := -1;
  emit(a div b);
end.
