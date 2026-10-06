program combo;
{ End-to-end exercise of four language features at once: a record-receiver method,
  overloaded procedures resolved by argument type, a set with membership, and a
  coroutine spawned with a seed argument and resumed across yields. Run under kprun it
  prints 7, 100, 1, 23. }
type
  Point = record x, y: integer; end;
  Small = set of 0..15;
procedure emit(v: integer); external;

{ a Go-style method on a record, receiver by const reference }
function sum for (const q: Point): integer;
begin
  sum := q.x + q.y;
end;

{ two overloads of 'show', chosen by the argument type }
procedure show(n: integer);
begin
  emit(n);
end;
procedure show(const p: Point);
begin
  emit(p.sum);
end;

{ a coroutine that yields a running total from its seed argument }
procedure counter(base: integer);
var i: integer;
begin
  for i := 1 to 3 do
    yield base + i;
end;

var
  p: Point;
  s: Small;
  k: process;
  acc: integer;
begin
  p.x := 3; p.y := 4;
  show(p);                             { Point overload calls p.sum -> 7 }
  show(100);                           { integer overload -> 100 }
  s := [2, 4, 6, 8];
  if 4 in s then show(1) else show(0); { set membership -> 1 }
  k := start counter(10);
  acc := 0;
  acc := acc + k(0);                   { first yield: 11 }
  acc := acc + k(0);                   { second yield: 12 }
  release(k);
  show(acc);                           { 23 }
end.
