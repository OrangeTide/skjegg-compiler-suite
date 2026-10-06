program mathfn;
{ The numeric built-ins. abs and sqr keep the argument's own type (integer,
  int64, or real); sqrt always yields a real, widening an integer argument;
  round converts a real to the nearest integer (round-to-nearest-even) and trunc
  toward zero. Run under kprun it prints the integers, then the int64, then the
  reals. }
procedure emit(v: integer); external;
procedure emit64(v: int64); external;
procedure emitf(v: real); external;

var
  i: integer;
  n: int64;
  r: real;
begin
  i := -7;
  emit(abs(i));          { 7 }
  emit(abs(5));          { 5 }
  emit(sqr(i));          { 49 }
  emit(sqr(6));          { 36 }
  emit(trunc(3.9));      { 3 }
  emit(trunc(-3.9));     { -3, real unary minus (FNEG) }
  emit(round(2.5));      { 2, nearest-even }
  emit(round(3.5));      { 4 }
  emit(round(-2.5));     { -2, real unary minus (FNEG) }

  n := -3000000000;
  emit64(abs(n));        { 3000000000, kept at 64 bits }

  emitf(abs(-2.5));      { 2.5, real unary minus (FNEG) }
  emitf(sqr(1.5));       { 2.25 }
  emitf(sqrt(16.0));     { 4 }
  r := 9.0;
  emitf(sqrt(r));        { 3 }
end.
