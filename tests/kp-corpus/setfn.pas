program setfn;
type Flags = set of 0..7;
var a, b: Flags;
procedure emit(v: integer); external;
function combine(x, y: Flags): Flags;
begin
  combine := x + y;
end;
begin
  a := [1, 2];
  b := [2, 3];
  if 3 in combine(a, b) then emit(1);
end.
