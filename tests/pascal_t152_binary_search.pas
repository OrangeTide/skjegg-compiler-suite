{ Test binary search }
program TestBinarySearch;
var
  a: array[1..10] of integer;
  i: integer;

function bsearch(var arr: array[1..10] of integer; target, lo, hi: integer): integer;
var
  mid: integer;
begin
  bsearch := -1;
  while lo <= hi do begin
    mid := (lo + hi) div 2;
    if arr[mid] = target then begin
      bsearch := mid;
      exit;
    end else if arr[mid] < target then
      lo := mid + 1
    else
      hi := mid - 1;
  end;
end;

begin
  for i := 1 to 10 do
    a[i] := i * 10;

  writeln(bsearch(a, 50, 1, 10));
  writeln(bsearch(a, 10, 1, 10));
  writeln(bsearch(a, 100, 1, 10));
  writeln(bsearch(a, 55, 1, 10));
end.
