program int64test;
{ Int64 is a distinct 64-bit integer, held and computed as a full 64-bit value
  through the *64 opcodes. A literal that overflows 32 bits is an Int64. The type
  works as a variable, an array element, a parameter, and a function result, and
  a 32-bit operand is sign-extended into an Int64 context. Run under kprun it
  prints 10000000000 15000000000 1099511627776 4 2000000000000000000
  13333333333 3333333333 -256 1 30000000000 42. }
procedure emit(v: integer); external;
procedure emit64(v: int64); external;

function sum64(a, b: int64): int64;
begin
  sum64 := a + b;
end;

function widen(n: integer): int64;
{ a 32-bit argument sign-extends into the 64-bit result }
begin
  widen := n;
end;

const
  Big = 10000000000;

var
  x, y: int64;
  arr: array[1..3] of int64;
  n: integer;
begin
  x := Big;
  emit64(x);                     { 10000000000 }
  y := x + 5000000000;
  emit64(y);                     { 15000000000 }

  x := 1;
  x := x shl 40;
  emit64(x);                     { 1099511627776 }

  emit64(Big mod 7);             { 4 }

  arr[1] := 1000000000;
  arr[2] := 2000000000;
  x := arr[1] * arr[2];          { 2e18, overflows 32 bits }
  emit64(x);                     { 2000000000000000000 }

  emit64(sum64(Big, Big div 3)); { 10000000000 + 3333333333 = 13333333333 }
  emit64(Big div 3);             { 3333333333 }

  x := -1024;
  emit64(x div 4);               { -256, signed }

  if Big > 5 then emit(1) else emit(0);   { 1, a 64-bit compare }

  n := 3;
  emit64(widen(n) * 10000000000);         { 30000000000 }

  emit(42);                      { 42, plain 32-bit still works }
end.
