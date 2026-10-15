program conf_trap_modovf;
{ Conformance: the most negative integer mod -1 overflows the same way (the
  divide step produces the unrepresentable quotient), so it traps OVERFLOW and
  exits 70 even though the mathematical remainder is 0. The operands are
  variables so it runs at runtime; the trap fires before emit. }
procedure emit(v: integer); external;

var
  a, b: integer;
begin
  a := -2147483647 - 1;
  b := -1;
  emit(a mod b);
end.
