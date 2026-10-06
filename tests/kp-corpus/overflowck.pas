{$Q+}
program overflowck;
procedure emit(v: integer); external;
var i, j: integer;
begin
  i := 30000;
  j := 3;
  emit(i * j);
end.
