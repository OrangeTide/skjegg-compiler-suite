program openarrayfwd;
{ A forward-declared routine with an open-array parameter links to its definition. Each
  `array of T` mints a distinct anonymous type, so the two headers must match structurally
  rather than by type id. Assigning the result inside the body then works, and the call
  resolves to the one linked symbol. Run under kprun it prints 42. }
procedure emit(v: integer); external;
function total(const a: array of integer): integer; forward;
var
  xs: array of integer;
function total(const a: array of integer): integer;
var
  i, s: integer;
begin
  s := 0;
  for i := 0 to High(a) do s := s + a[i];
  total := s;
end;
begin
  SetLength(xs, 3);
  xs[0] := 10;
  xs[1] := 20;
  xs[2] := 12;
  emit(total(xs));            { 42 }
end.
