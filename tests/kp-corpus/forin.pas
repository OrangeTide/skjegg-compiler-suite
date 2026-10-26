program forin;
{ for-in over every backing: a fixed array (its index range), a dynamic array
  (0..high), a string (its bytes), a map (key, value), a set (members in order),
  and a custom cursor (a Next-fails method). The _ binder ignores an element
  while still running the body once per element. Prints the sequence in
  forin.expected. }
procedure emit(v: integer); external;
type
  Triple = array[1..3] of integer;
  Counter = record
    cur, stop: integer;
  end;
function Next for (ct: Counter): integer fails;
begin
  if ct.cur >= ct.stop then fail(1);
  Next := ct.cur;
  ct.cur := ct.cur + 1;
end;
var
  fixed: Triple;
  dyn: array of integer;
  s: string;
  m: map of integer;
  bits: set of 0..63;
  ctr: Counter;
  k: string;
  x, i, v, sum, cnt: integer;
  c: char;
begin
  fixed[1] := 10; fixed[2] := 20; fixed[3] := 30;
  for x in fixed do emit(x);           { 10 20 30 }

  SetLength(dyn, 3);
  for i := 0 to High(dyn) do dyn[i] := (i + 1) * (i + 1);
  for x in dyn do emit(x);             { 1 4 9 }

  s := 'abc';
  for c in s do emit(Ord(c));          { 97 98 99 }

  for _ in fixed do emit(7);           { 7 7 7 }

  { a map binds key and value; iteration order is unspecified, so the test sums }
  m['a'] := 1; m['b'] := 2; m['c'] := 3;
  sum := 0; cnt := 0;
  for k, v in m do begin sum := sum + v; cnt := cnt + length(k); end;
  emit(sum);                           { 6 }
  emit(cnt);                           { 3 }
  cnt := 0;
  for k in m do cnt := cnt + 1;        { key only }
  emit(cnt);                           { 3 }
  sum := 0;
  for _, v in m do sum := sum + v;     { value only }
  emit(sum);                           { 6 }

  { a set iterates its members in ordinal order, low to high }
  bits := [2, 5, 40];
  for x in bits do emit(x);            { 2 5 40 }

  { a custom cursor: Next yields until it fails }
  ctr.cur := 0; ctr.stop := 3;
  for x in ctr do emit(x);             { 0 1 2 }
end.
