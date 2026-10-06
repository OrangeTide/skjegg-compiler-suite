program i64test;
var
    a, b: int64;
begin
    a := 1000000000;
    b := a * a;
    WriteLn('a = ', a);
    WriteLn('b = ', b);
    WriteLn('b div a = ', b div a);
end.
