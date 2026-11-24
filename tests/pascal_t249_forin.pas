{ for..in loops over static arrays: iterate the elements of an array, binding
  the loop variable to each in turn.  It desugars to a counted loop over the
  array's index range, so it works for any element type (integer, char, a
  record) and honours break and continue. }
program ForInDemo;
type
  pt = record x, y: integer end;
var
  a: array[1..5] of integer;
  letters: array[0..2] of char;
  pts: array[1..3] of pt;
  i, v, sum, total: integer;
  c: char;
  p: pt;
begin
  for i := 1 to 5 do a[i] := i * i;
  sum := 0;
  for v in a do sum := sum + v;
  writeln(sum);                 { 1+4+9+16+25 = 55 }

  letters[0] := 'X';
  letters[1] := 'Y';
  letters[2] := 'Z';
  for c in letters do writeln(c);

  pts[1].x := 1; pts[1].y := 10;
  pts[2].x := 2; pts[2].y := 20;
  pts[3].x := 3; pts[3].y := 30;
  for p in pts do writeln(p.x, ' ', p.y);

  a[4] := 99;
  total := 0;
  for v in a do begin
    if v = 99 then break;       { break leaves the loop early }
    total := total + v
  end;
  writeln(total)                { 1 + 4 + 9 = 14 }
end.
