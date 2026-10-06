program transc;
{ The transcendental builtins: sin, cos, exp, ln, and arctan, each a one-argument
  real function lowered to a runtime helper over the C library. An integer argument
  is widened to real (cos(3)). Run under kprun it prints
  0 1 1 0 0 2.71828 2 0.841471 0.785398 -0.989992. }
procedure emitf(v: real); external;

begin
  emitf(sin(0.0));       { 0 }
  emitf(cos(0.0));       { 1 }
  emitf(exp(0.0));       { 1 }
  emitf(ln(1.0));        { 0 }
  emitf(arctan(0.0));    { 0 }
  emitf(exp(1.0));       { 2.71828, e }
  emitf(ln(exp(2.0)));   { 2, ln and exp invert }
  emitf(sin(exp(0.0)));  { 0.841471, sin(1) }
  emitf(arctan(1.0));    { 0.785398, pi/4 }
  emitf(cos(3));         { -0.989992, integer argument widened }
end.
