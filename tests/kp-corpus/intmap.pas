program intmaptest;
procedure emit(v: integer); external;
var
  m: map[integer] of integer;
  c: map[char] of integer;
  w: map[int64] of integer;
  k: integer;
  ck: char;
  wk: int64;
  v, total: integer;
begin
  { integer keys, including 0 and 1 (would be empty/tomb sentinels for strings) }
  m[0] := 10; m[1] := 20; m[42] := 30; m[-7] := 40;
  emit(Length(m));
  emit(m[0]); emit(m[1]); emit(m[42]); emit(m[-7]);
  if 42 in m then emit(100);
  if not (99 in m) then emit(200);
  Delete(m, 1);
  emit(Length(m));
  if not (1 in m) then emit(300);
  if TryGet(m, 42, v) then emit(v);
  if not TryGet(m, 1, v) then emit(400);
  m[0] := m[0] + 5;
  emit(m[0]);
  total := 0;
  for k, v in m do total := total + k + v;
  emit(total);

  { char keys }
  c['a'] := 1; c['z'] := 26;
  emit(Length(c));
  emit(c['a']); emit(c['z']);
  if 'a' in c then emit(500);
  total := 0;
  for ck, v in c do total := total + v;
  emit(total);

  { int64 keys with a value past 32 bits }
  w[5000000000] := 7; w[1] := 8;
  emit(Length(w));
  emit(w[5000000000]); emit(w[1]);
  total := 0;
  for wk, v in w do total := total + v;
  emit(total);
end.
