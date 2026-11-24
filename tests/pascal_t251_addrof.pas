{ @x takes the address of a variable, field or element as a typed pointer, the
  inverse of p^.  Writing through the pointer aliases the original storage. }
program AddrOf;
type
  PInt = ^integer;
  Rec = record a: integer; b: integer end;
var
  x: integer;
  p: PInt;
  r: Rec;
  arr: array[1..3] of integer;
  i: integer;
begin
  x := 10;
  p := @x;
  p^ := p^ + 5;
  writeln(x);                 { 15: the write went through @x }

  r.a := 1; r.b := 2;
  p := @r.b;
  p^ := 99;
  writeln(r.a, ' ', r.b);     { 1 99 }

  for i := 1 to 3 do arr[i] := i;
  p := @arr[2];
  p^ := 42;
  writeln(arr[1], ' ', arr[2], ' ', arr[3])   { 1 42 3 }
end.
