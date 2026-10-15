program rconst;
{ Real and single-precision constants: a scalar real const (inferred and explicitly typed),
  a single scalar const, an integer value promoted to a real-typed const, a constant array of
  single, and a record constant with real and single fields. A const may also fill an element
  of another const (Quarter inside Tbl and the record). A single stores 4 bytes narrowed from
  the f64 the value computes in. Run under kprun it prints 0.75 0.5 3.5 1.5 2.25 0.25 0.5 0.25,
  one per line. }
procedure emitf(v: real); external;
procedure emit(v: integer); external;
const
  Quarter = 0.25;
  Half: single = 0.5;
  Three: real = 3;
  Tbl: array[0..2] of single = (1.5, 2.25, Quarter);
type
  Vec = record x: real; s: single; end;
const
  V: Vec = (x: Half; s: Quarter);
var
  i: integer;
begin
  emitf(Quarter + Half);
  emitf(Half);
  emitf(Three + 0.5);
  for i := 0 to 2 do emitf(Tbl[i]);
  emitf(V.x);
  emitf(V.s);
end.
