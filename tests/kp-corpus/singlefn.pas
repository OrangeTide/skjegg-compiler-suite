program singlefn;
{ Single-precision parameters and function results cross the call boundary as
  f32 in the SSE registers. The caller narrows each single argument (FARG imm=4)
  and widens a single result after the call (FCALL imm=4); the callee stores a
  single parameter with movss (param_fw) and narrows its single result at FRETV.
  Computation stays in f64. 16777217 (2^24 + 1) is not representable in f32, so a
  value that round-trips through a single parameter or result rounds to 16777216.
  Run under kprun it prints 6 0 5. }
procedure emitf(v: real); external;

function addf(a: single; b: single): single;
begin
  addf := a + b;
end;

function trunc24(x: single): single;
{ x arrives already narrowed to single, so 2^24+1 became 2^24 on the way in. }
begin
  trunc24 := x;
end;

var
  s: single;
begin
  s := addf(2.5, 3.5);
  emitf(s);                       { 6 }
  emitf(trunc24(16777217.0) - 16777216.0);  { 0, narrowed at the parameter }
  emitf(addf(1.5, 3.5));          { 5 }
end.
