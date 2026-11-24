{ Transcendental real builtins: sqrt, exp, ln, sin, cos, arctan.  Printed in
  fixed point so the output is deterministic.  Runs on the host backends, where
  the runtime has a real libc math library. }
program MathFn;
begin
  writeln(sqrt(2.0):0:5);
  writeln(exp(1.0):0:5);
  writeln(ln(exp(1.0)):0:5);
  writeln(sin(0.0):0:5);
  writeln(cos(0.0):0:5);
  writeln(arctan(1.0):0:5)
end.
