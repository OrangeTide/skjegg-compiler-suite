program largeset;
{ Large sets: a set whose base type needs more than 64 bits is a 2-to-4 word blob addressed in
  memory, not a register i64. Construction, membership, union/intersection/difference, and the
  relations all lower to inline word-by-word operations through the base address. This set of
  0..127 is two words, so it exercises elements on both sides of the 64-bit boundary. Run under
  kprun it prints 1 0 1 1 1 0 1 1 0 1 1. }
procedure emit(v: integer); external;

type
  Big = 0..127;

var
  s, t: set of Big;
  x: integer;
begin
  s := [1, 65, 100, 127];
  emit(Ord(1 in s));      { 1 }
  emit(Ord(64 in s));     { 0, the empty low-of-second-word bit }
  emit(Ord(65 in s));     { 1 }
  emit(Ord(127 in s));    { 1, the top bit }

  t := [65, 66];
  s := s * t;             { intersection is the single element 65 }
  emit(Ord(65 in s));     { 1 }
  emit(Ord(100 in s));    { 0 }

  s := [1, 65];
  t := [1, 65];
  emit(Ord(s = t));       { 1 }
  t := [1];
  emit(Ord(t <= s));      { 1, subset }
  emit(Ord(s <= t));      { 0 }

  x := 100;
  s := [x, 5];            { a runtime element indexed by word and bit }
  emit(Ord(100 in s));    { 1 }
  emit(Ord(5 in s));      { 1 }
end.
