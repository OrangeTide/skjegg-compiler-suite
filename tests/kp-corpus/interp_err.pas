program interp_err;
{ A real cannot be interpolated yet; this fixture must be rejected (interp_err.experror). }
var
  r: real;
begin
  r := 3.5;
  WriteLn("value is ${r}");
end.
