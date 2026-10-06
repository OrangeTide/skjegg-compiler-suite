{$R+}
program rangecheck;
type Vec = array[1..3] of integer;
procedure emit(v: integer); external;
var a: Vec; i: integer;
begin
  i := 2;
  a[i] := 7;
  emit(a[i]);
end.
