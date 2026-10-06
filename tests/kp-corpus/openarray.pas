program openarray;
{ Open-array parameters. A `const a: array of integer` parameter is a dynamic array,
  so a dynamic-array argument passes straight through, the empty constructor [] passes
  as an empty array, and nil passes as an empty array. [] and nil also assign to a
  dynamic-array variable. Run under kprun it prints 60 0 3 0 0 0. }
procedure emit(v: integer); external;

type
  TIntArr = array of integer;

function total(const a: array of integer): integer;
var
  i, s: integer;
begin
  s := 0;
  for i := 0 to High(a) do s := s + a[i];
  total := s;
end;

var
  d, e: TIntArr;
begin
  SetLength(d, 3);
  d[0] := 10; d[1] := 20; d[2] := 30;
  emit(total(d));      { 60, a dynamic array passes to an open-array parameter }
  emit(total([]));     { 0, the empty constructor is an empty array }
  emit(Length(d));     { 3 }
  d := [];             { [] assigns to a dynamic-array variable }
  emit(Length(d));     { 0 }
  e := nil;            { nil assigns to a dynamic-array variable }
  emit(Length(e));     { 0 }
  emit(total(nil));    { 0, nil passes to an open-array parameter }
end.
