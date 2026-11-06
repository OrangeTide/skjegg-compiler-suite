program arrcapgrow;
{ Coverage for Delete/Insert followed by a SetLength grow: the corpus exercised
  Delete-then-Insert (arredit) but never a grow after an edit, which is the path
  that reads the dynarr capacity field. The runtime must track capacity across
  Delete and Insert so the grow reallocates against the real storage size. The
  current gheap never shrinks a block in place, so a stale capacity happens to
  stay within bounds there; this guards the values, and the capacity bookkeeping
  stays correct for a realloc that does shrink. }
procedure emit(v: integer); external;
var
  a: array of integer;
  i: integer;
begin
  SetLength(a, 10);
  for i := 0 to 9 do a[i] := i + 1;        { 1 2 3 4 5 6 7 8 9 10 }
  Delete(a, 0, 8);                          { 9 10 ; storage reallocated to 2 }
  emit(Length(a));                          { 2 }
  emit(a[0]); emit(a[1]);                   { 9 10 }
  SetLength(a, 9);                          { grow 2 -> 9 ; must realloc, not overrun }
  emit(Length(a));                          { 9 }
  for i := 0 to Length(a) - 1 do emit(a[i]);{ 9 10 0 0 0 0 0 0 0 }

  { Insert grows the storage too: its capacity must match so the next grow is sound. }
  SetLength(a, 2);                          { 9 10 ; shrink keeps storage }
  Insert(a, 1, 1);                          { 9 _ 10 ; storage reallocated to 3 }
  a[1] := 5;                                { 9 5 10 }
  SetLength(a, 6);                          { grow 3 -> 6 ; must realloc, not overrun }
  emit(Length(a));                          { 6 }
  for i := 0 to Length(a) - 1 do emit(a[i]);{ 9 5 10 0 0 0 }
end.
