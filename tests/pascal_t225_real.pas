program pascal_t225_real;
type
  TPoint = record
    x, y: Double;
  end;
var
  a, b: Double;
  s: Single;
  p: TPoint;
  arr: array[0..2] of Double;
  i: Integer;

function Hypot2(u, v: Double): Double;
begin
  Hypot2 := u * u + v * v;
end;

function Half(v: Double): Single;
begin
  Half := v / 2.0;
end;

begin
  a := 3.5;
  b := 2.0;
  writeln(a + b);
  writeln(a * b);
  writeln(a - b);
  writeln(a / b);
  writeln(Hypot2(3.0, 4.0));
  s := Half(9.0);
  writeln(s);
  a := 10;            { int to real coercion }
  writeln(a);
  writeln(trunc(3.9));
  writeln(round(3.5));
  writeln(round(2.4));
  writeln(sqrt(16.0));
  p.x := 1.5;
  p.y := 2.5;
  writeln(p.x + p.y);
  for i := 0 to 2 do
    arr[i] := i * 1.5;
  for i := 0 to 2 do
    writeln(arr[i]);
  if a > b then writeln(1) else writeln(0);
  if b >= 2.0 then writeln(1) else writeln(0);
end.
