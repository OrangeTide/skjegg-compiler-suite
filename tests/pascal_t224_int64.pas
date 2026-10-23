program pascal_t224_int64;
type
  TRec = record
    a: Int64;
    b: Integer;
  end;
var
  x, y: Int64;
  r: TRec;
  arr: array[0..3] of Int64;
  i: Integer;

function Add64(p, q: Int64): Int64;
begin
  Add64 := p + q;
end;

function Big: Int64;
begin
  Big := Int64(1) shl 40;
end;

begin
  x := 1000000000;
  y := x * x;           { 10^18, overflows 32-bit }
  writeln(y);
  writeln(x + 5);
  writeln(Add64(y, 1));
  writeln(Big);
  writeln(Int64(1) shl 63);        { most negative }
  writeln(-(Int64(1) shl 63));     { wraps to itself }
  x := Int64(1) shl 62;
  writeln(x and (not (Int64(1) shl 62)));   { 0 }
  r.a := y;
  r.b := 7;
  writeln(r.a);
  writeln(r.b);
  for i := 0 to 3 do
    arr[i] := Int64(i) * 3000000000;
  for i := 0 to 3 do
    writeln(arr[i]);
  if y > x then writeln(1) else writeln(0);
end.
