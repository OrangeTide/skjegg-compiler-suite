program t044_array_param;
type
  TArr = array[1..4] of integer;

function SumArr(const a: TArr): integer;
var
  i, s: integer;
begin
  s := 0;
  for i := 1 to 4 do
    s := s + a[i];
  SumArr := s;
end;

procedure FillArr(var a: TArr; v: integer);
var
  i: integer;
begin
  for i := 1 to 4 do
    a[i] := v + i;
end;

procedure ZeroArr(a: TArr);
var
  i: integer;
begin
  { Value param: should not affect caller }
  for i := 1 to 4 do
    a[i] := 0;
  writeln(SumArr(a));
end;

var
  arr: TArr;
begin
  FillArr(arr, 10);
  writeln(SumArr(arr));
  ZeroArr(arr);
  writeln(SumArr(arr));
end.
