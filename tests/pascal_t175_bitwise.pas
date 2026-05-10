{ Test bitwise operations }
program TestBitwise;
var
  a, b: integer;
begin
  a := 255;
  b := 170;

  writeln(a and b);
  writeln(a or b);

  { mask extraction }
  a := 12345;
  writeln(a and 255);
  writeln((a shr 8) and 255);

  { set bit }
  a := 0;
  a := a or (1 shl 3);
  a := a or (1 shl 5);
  writeln(a);

  { toggle bits with xor-like: a and (not b) or (not a and b) }
  a := 15;
  b := a and 255;
  writeln(b);

  { shift patterns }
  a := 1;
  b := 0;
  while a <= 128 do begin
    b := b or a;
    a := a shl 1;
  end;
  writeln(b);
end.
