{ Test array reversal in-place }
program TestArrayReverse;
var
  a: array[1..8] of integer;
  i, tmp, lo, hi: integer;
begin
  for i := 1 to 8 do
    a[i] := i * 10;

  lo := 1;
  hi := 8;
  while lo < hi do begin
    tmp := a[lo];
    a[lo] := a[hi];
    a[hi] := tmp;
    lo := lo + 1;
    hi := hi - 1;
  end;

  for i := 1 to 8 do
    writeln(a[i]);
end.
