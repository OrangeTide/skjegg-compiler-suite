program dynarray;
{ Dynamic arrays end to end: SetLength allocates, indexed assignment fills, Length and
  High report the size, and a grow zero-fills the new tail. Run under kprun it prints
  5 1 4 9 16 25 7 0 42. }
procedure emit(v: integer); external;
var
  a: array of integer;
  i: integer;
begin
  SetLength(a, 5);
  for i := 0 to High(a) do
    a[i] := (i + 1) * (i + 1);
  emit(Length(a));
  for i := 0 to Length(a) - 1 do
    emit(a[i]);
  SetLength(a, 7);           { grow: the new tail is zero-filled }
  emit(Length(a));
  emit(a[6]);
  a[6] := 42;
  emit(a[6]);
end.
