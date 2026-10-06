program procint;
procedure emit(v: integer); external;
function dbl(x: integer): integer;
begin
  dbl := x * 2;
end;
function incr(x: integer): integer;
begin
  incr := x + 1;
end;
procedure apply(f: function (x: integer): integer; n: integer);
begin
  emit(f(n));
end;
var cb: function (x: integer): integer;
begin
  cb := dbl;
  emit(cb(5));
  cb := incr;
  emit(cb(5));
  apply(dbl, 20);
  apply(incr, 20);
end.
