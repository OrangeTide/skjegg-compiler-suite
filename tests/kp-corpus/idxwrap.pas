program idxwrap;
{ Regression: an inline array index computed with a negative operand must not
  leak high bits into the address. c + nb with nb = -1 is the in-range index 2,
  but nb sits in a register as 0xFFFFFFFF; the i32 add has to run at 32-bit
  width so the sum stays a clean 32-bit value and the address stays in the
  low-4GB range. Run under kprun it prints 12. }
procedure emit(v: integer); external;
var
  a: array[0..3] of integer;
  c, nb: integer;
begin
  a[0] := 10;
  a[1] := 11;
  a[2] := 12;
  a[3] := 13;
  c := 3;
  nb := -1;
  emit(a[c + nb]);
end.
