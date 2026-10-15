program conf_divmod;
{ Conformance: integer div truncates toward zero and mod takes the sign of the
  dividend, for both integer and int64 (kp-evaluation.md, "Integer arithmetic").
  Not ISO Pascal floored division. Variables force the runtime path rather than
  constant folding. Run under kprun it prints
  -3 -1 -3 1 3 -1 3 1 -3 -1 -3 1. }
procedure emit(v: integer); external;
procedure emit64(v: int64); external;

var
  a, b: integer;
  p, q: int64;
begin
  a := -7; b := 2;
  emit(a div b);     { -3 }
  emit(a mod b);     { -1 }
  a := 7; b := -2;
  emit(a div b);     { -3 }
  emit(a mod b);     { 1 }
  a := -7; b := -2;
  emit(a div b);     { 3 }
  emit(a mod b);     { -1 }
  a := 7; b := 2;
  emit(a div b);     { 3 }
  emit(a mod b);     { 1 }

  p := -7; q := 2;
  emit64(p div q);   { -3 }
  emit64(p mod q);   { -1 }
  p := 7; q := -2;
  emit64(p div q);   { -3 }
  emit64(p mod q);   { 1 }
end.
