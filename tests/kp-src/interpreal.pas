program interpreal;
{ Real interpolation holes render the same way write renders a real (the shared
  %g formatter), so the interpolated line and the direct write agree.  Runs on
  every tier, the freestanding one included. }
var
  r: real;
  s: single;
begin
  r := 3.14159;
  s := 2.5;
  WriteLn("r=${r} half=${r / 2} s=${s} mix=${r}/${s}");
  Write('direct: '); Write(r); Write(' '); WriteLn(s);
end.
