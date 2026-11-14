program interp_err;
{ A record has no write rendering, so it cannot be interpolated into a string;
  this fixture must be rejected (interp_err.experror). A real now interpolates
  (see interp.pas). }
type
  Point = record x, y: integer; end;
var
  p: Point;
begin
  p.x := 1;
  p.y := 2;
  WriteLn("point is ${p}");
end.
