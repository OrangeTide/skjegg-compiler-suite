program di;
procedure emit(v: integer); external;
var
  a: array of integer;
  i: integer;
begin
  SetLength(a, 5);
  for i := 0 to 4 do a[i] := i + 1;      { 1 2 3 4 5 }
  Delete(a, 1, 2);                        { 1 4 5 }
  emit(Length(a));                        { 3 }
  for i := 0 to Length(a) - 1 do emit(a[i]);  { 1 4 5 }
  Insert(a, 1, 2);                        { 1 _ _ 4 5 }
  a[1] := 8; a[2] := 9;                   { 1 8 9 4 5 }
  emit(Length(a));                        { 5 }
  for i := 0 to Length(a) - 1 do emit(a[i]);  { 1 8 9 4 5 }
  Delete(a, 3, 100);                      { clamp: 1 8 9 }
  emit(Length(a));                        { 3 }
end.
