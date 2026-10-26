program forin_err;
{ for-in needs a sequence, set, map, or a type with a Next-fails method. A plain
  record without such a method cannot be iterated. This fixture must be rejected;
  see forin_err.experror. }
procedure emit(v: integer); external;
type
  Plain = record
    a, b: integer;
  end;
var
  p: Plain;
  x: integer;
begin
  p.a := 1; p.b := 2;
  for x in p do emit(x);
end.
