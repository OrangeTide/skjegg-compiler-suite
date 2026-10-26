program defer_err;
{ A deferred statement may not transfer control out of its block, so `break`
  inside a `defer` is a compile error (kp-defer.md). This fixture must be
  rejected; see defer_err.experror. }
procedure emit(v: integer); external;
var
  i: integer;
begin
  for i := 1 to 3 do begin
    defer break;
    emit(i);
  end;
end.
