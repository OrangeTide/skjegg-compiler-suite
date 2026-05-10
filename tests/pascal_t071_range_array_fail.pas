{$R+}
program RangeArrayFail;
var
  a: array[1..5] of integer;
begin
  a[1] := 10;
  a[6] := 99
end.
