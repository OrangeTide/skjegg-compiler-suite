program mtest;
procedure emit(v: integer); external;
var
  m: map of integer;
  it: mapiter;
  k: string;
  v, total: integer;
begin
  m['a'] := 1; m['b'] := 2; m['c'] := 3;
  emit(Length(m));
  if 'b' in m then emit(100);
  if not ('z' in m) then emit(200);
  Delete(m, 'b');
  emit(Length(m));
  if TryGet(m, 'a', v) then emit(v);
  if not TryGet(m, 'b', v) then emit(300);
  total := 0;
  it := MapFirst(m);
  while MapNext(it, k, v) do total := total + v;
  emit(total);
end.
