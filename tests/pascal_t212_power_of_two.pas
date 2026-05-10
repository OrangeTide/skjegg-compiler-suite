{ Test powers of two }
program TestPowerOfTwo;
var
  n, i: integer;

function isPowerOf2(x: integer): boolean;
var
  v: integer;
begin
  v := 1;
  while v < x do
    v := v * 2;
  isPowerOf2 := (v = x);
end;

begin
  { Print powers of 2 up to 256 }
  n := 1;
  for i := 1 to 9 do begin
    writeln(n);
    n := n * 2;
  end;

  { Test isPowerOf2 }
  if isPowerOf2(64) then writeln(1) else writeln(0);
  if isPowerOf2(100) then writeln(1) else writeln(0);
  if isPowerOf2(1) then writeln(1) else writeln(0);
end.
