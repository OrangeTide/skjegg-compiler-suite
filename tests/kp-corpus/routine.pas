program routine;
var n: integer;
procedure emit(v: integer); external;
function fact(k: integer): integer;
var acc, j: integer;
begin
  acc := 1;
  for j := k downto 1 do
    acc := acc * j;
  fact := acc;
end;
begin
  for n := 1 to 5 do
    emit(fact(n));
end.
