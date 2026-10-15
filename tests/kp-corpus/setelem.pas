program setelem;
{ A set as a dynamic-array element and a map value. A set is reference-free, so it carries no
  per-element handle layout: a small set (set of 0..7) rides the 64-bit scalar element slot, a
  large set (set of 0..199) blits into the inline aggregate slot the way a record does. Value
  semantics hold: a set written into a slot is an independent copy, so a later change to the
  source does not reach the stored element. Run under kprun it prints
  1 0 1 1 1 0 1 3 1 0 1 1, one per line. }
procedure emit(v: integer); external;

type
  Small = set of 0..7;
  Large = set of 0..199;

var
  sa: array of Small;
  la: array of Large;
  sm: map of Small;
  lm: map of Large;
  s: Small;
  l: Large;
begin
  { --- dynamic array of a small set (scalar element slot) --- }
  SetLength(sa, 2);
  sa[0] := [1, 3, 5];
  sa[1] := [2];
  if 3 in sa[0] then emit(1) else emit(0);     { 1 }
  if 3 in sa[1] then emit(1) else emit(0);     { 0 }
  { value independence: write a set, then change the source; the element keeps its value }
  s := [7];
  sa[1] := s;
  s := [0];
  if 7 in sa[1] then emit(1) else emit(0);     { 1, the element kept [7] }
  if 0 in sa[1] then emit(0) else emit(1);     { 1, the source change did not leak in }

  { --- dynamic array of a large set (inline aggregate slot, blit) --- }
  SetLength(la, 2);
  la[0] := [10, 150, 199];
  la[1] := [];
  if 150 in la[0] then emit(1) else emit(0);   { 1 }
  if 150 in la[1] then emit(1) else emit(0);   { 0 }
  l := [5, 199];
  la[1] := l;                                  { whole-element blit }
  if 199 in la[1] then emit(1) else emit(0);   { 1 }

  { --- maps of a small and a large set --- }
  sm['a'] := [2, 4];
  sm['b'] := [];
  lm['x'] := [100, 101];
  emit(Length(sm) + Length(lm));               { 3 (two small-set keys, one large-set key) }
  if 4 in sm['a'] then emit(1) else emit(0);   { 1 }
  if 4 in sm['b'] then emit(1) else emit(0);   { 0 }
  if 100 in lm['x'] then emit(1) else emit(0); { 1 }
  lm['x'] := [199];                            { overwrite a large-set map value }
  if 199 in lm['x'] then emit(1) else emit(0); { 1 }
end.
