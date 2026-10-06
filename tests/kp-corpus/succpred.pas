program succpred;
{ succ/pred yield the next and previous ordinal as a value; odd tests the low bit.
  succ/pred carry the argument's own ordinal type, so they work on integers, int64,
  enumerations, and char. Run under kprun it prints 6 4 10 1 1 1 66 1 0 0 1. }
procedure emit(v: integer); external;

type Color = (Red, Green, Blue);

var
  i: integer;
  n: int64;
  c: Color;
  ch: char;
begin
  i := 5;
  emit(succ(i));            { 6 }
  emit(pred(i));            { 4 }
  emit(succ(i) + pred(i));  { 10 }

  n := 1000000000000;
  emit(succ(n) - n);        { 1, kept at 64 bits }

  c := Red;
  c := succ(c);
  emit(ord(c));             { 1 = Green }
  emit(ord(pred(Blue)));    { 1 = Green }

  ch := 'A';
  emit(ord(succ(ch)));      { 66 = 'B' }

  if odd(i) then emit(1) else emit(0);     { 1 }
  if odd(4) then emit(1) else emit(0);     { 0 }
  if odd(n) then emit(1) else emit(0);     { 0 }
  if odd(n + 1) then emit(1) else emit(0); { 1 }
end.
