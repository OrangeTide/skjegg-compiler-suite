program sets;
type Color = (Red, Green, Blue, Cyan, Magenta, Yellow);
var
  s, t, u: set of Color;
  digits: set of 0..9;
  ok: boolean;
procedure emit(v: integer); external;
begin
  s := [Red, Blue, Yellow];
  t := [Green..Magenta];
  u := s + t;
  u := u * s;
  u := u - t;
  digits := [1, 3..5, 8];
  ok := (Red in s) and (t <= u) and (u >= t) and (s = t);
  if 4 in digits then emit(1);
end.
