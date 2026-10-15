program conf_defaults;
{ Conformance: a global variable with no initializer reads as the zero of its
  type (kp-evaluation.md, "Default and uninitialized values"): integer 0, boolean
  false, char #0, real 0.0, an empty set, and a record whose fields are each
  zero. (Non-handle locals are undefined, so only globals are pinned here.) The
  real default is checked as gr + 0.5 to keep the printed form stable. Run under
  kprun it prints 0 0 0 0.5 0 0 0. }
procedure emit(v: integer); external;
procedure emitf(x: real); external;

type
  Rec = record a, b: integer end;

var
  gi: integer;
  gb: boolean;
  gc: char;
  gr: real;
  gs: set of 0..7;
  grec: Rec;
begin
  emit(gi);              { 0 }
  emit(Ord(gb));         { 0, false }
  emit(Ord(gc));         { 0, #0 }
  emitf(gr + 0.5);       { 0.5, so gr is 0.0 }
  emit(Ord(5 in gs));    { 0, empty set }
  emit(grec.a);          { 0 }
  emit(grec.b);          { 0 }
end.
