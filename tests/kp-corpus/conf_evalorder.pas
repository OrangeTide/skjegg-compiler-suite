program conf_evalorder;
{ Conformance: operator operands evaluate left to right, call arguments right to
  left (kp-evaluation.md, "Evaluation order"). tag(n) appends n to a decimal
  trace and returns n, so the trace records the real order. The subtraction
  evaluates its left operand first (trace 12); the two-argument call evaluates
  its arguments last-to-first (trace 43). Run under kprun it prints -1 12 43. }
procedure emit(v: integer); external;

var
  seq: integer;

function tag(n: integer): integer;
begin
  seq := seq * 10 + n;
  tag := n;
end;

procedure two(_a, _b: integer);
begin
end;

begin
  seq := 0;
  emit(tag(1) - tag(2));   { value -1; left-then-right makes seq 12 }
  emit(seq);               { 12 }

  seq := 0;
  two(tag(3), tag(4));     { right-then-left makes seq 43 }
  emit(seq);               { 43 }
end.
