program casts;
{ Integer type conversions. Int64(x) widens a 32-bit value so an expression computes in
  64 bits (a shift base or a product that would overflow 32 bits). LongInt(x) narrows an
  Int64 to its low 32 bits, so later arithmetic runs 32-bit. A same-type conversion is the
  identity. Run under kprun it prints 1099511627776 6000000000 1410065408 201437915 42. }
procedure emit(v: integer); external;
procedure emit64(v: int64); external;

var
  v: int64;
  i: integer;
begin
  emit64(Int64(1) shl 40);        { 1099511627776, the base widened before the shift }
  emit64(Int64(3) * 2000000000);  { 6000000000, the product exceeds 32 bits }

  v := 10000000000;
  emit(LongInt(v));               { 1410065408, the low 32 bits }
  i := LongInt(v) div 7;          { narrowed, then a 32-bit divide }
  emit(i);                        { 201437915 }

  emit(Integer(42));              { 42, a same-type conversion is the identity }
end.
