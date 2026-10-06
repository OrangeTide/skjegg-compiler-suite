program localagg;
type Vec = array[1..3] of integer;
procedure emit(v: integer); external;
function dot(const u, w: Vec): integer;
var acc, k: integer; tmp: Vec;
begin
  tmp := u;
  acc := 0;
  for k := 1 to 3 do
    acc := acc + tmp[k] * w[k];
  dot := acc;
end;
var a, b: Vec; i: integer;
begin
  for i := 1 to 3 do begin
    a[i] := i;
    b[i] := 10 - i;
  end;
  emit(dot(a, b));
end.
