program conf_wrap;
{ Conformance: signed integer add, subtract, and multiply wrap in two's
  complement by default (overflow checking is the opt-in Q+ switch, off here),
  for both integer and int64 (kp-evaluation.md, "Overflow"). Variables force the
  runtime op rather than constant folding. Run under kprun it prints
  -2147483648 2147483647 0 -9223372036854775808. }
procedure emit(v: integer); external;
procedure emit64(v: int64); external;

var
  a: integer;
  p: int64;
begin
  a := 2147483647;         { the 32-bit maximum }
  a := a + 1;
  emit(a);                 { -2147483648, wraps }
  a := a - 1;
  emit(a);                 { 2147483647, wraps back }
  a := 65536;
  a := a * a;              { 2^32 keeps only its low 32 bits }
  emit(a);                 { 0 }

  p := 9223372036854775807;  { the 64-bit maximum }
  p := p + 1;
  emit64(p);               { -9223372036854775808, wraps }
end.
