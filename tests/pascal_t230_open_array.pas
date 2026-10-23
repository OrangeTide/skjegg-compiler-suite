{ Open-array parameters and nested dynamic arrays (P3 stage C).  An open-array
  `const` parameter (array of T) accepts a dynamic array, another open array
  passed through, or an array constructor ([] or [e, ...]).  A nested dynamic
  array (array of array of T) stores inner descriptors as elements; a local one
  releases those inner arrays recursively on scope exit. }
program OpenArray;
type
  TIntArray = array of LongInt;
  TMatrix = array of array of LongInt;

function total(const a: array of LongInt): LongInt;
var
  i, s: LongInt;
begin
  s := 0;
  for i := 0 to High(a) do
    s := s + a[i];
  total := s;
end;

function total2(const a: array of LongInt): LongInt;
begin
  total2 := total(a);          { hand one open array to another }
end;

function gridsum: LongInt;
var
  g: TMatrix;
  i, j, s: LongInt;
begin
  SetLength(g, 3);
  for i := 0 to 2 do begin
    SetLength(g[i], 3);
    for j := 0 to 2 do
      g[i][j] := i * 3 + j;
  end;
  s := 0;
  for i := 0 to 2 do
    for j := 0 to 2 do
      s := s + g[i][j];
  gridsum := s;                { g (and its inner arrays) freed on exit }
end;

var
  v: TIntArray;
  i: integer;
begin
  SetLength(v, 4);
  for i := 0 to 3 do
    v[i] := (i + 1) * 10;
  writeln(total(v));           { dynamic array argument }
  writeln(total2(v));          { pass-through }
  writeln(total([]));          { empty constructor }
  writeln(total([5, 15, 25])); { non-empty constructor }
  writeln(gridsum);
end.
