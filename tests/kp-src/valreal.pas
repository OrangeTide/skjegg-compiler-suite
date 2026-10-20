program valreal;
{ Val parsing a string into a real, the freestanding tier's scan_real (shared
  with libc's strtod).  The hosted tiers parse with the C library's strtod, so
  all tiers must agree on these. }
var
  r: real;
  code: integer;
begin
  Val('3.5', r, code);
  WriteLn(r);
  Val('1.5e2', r, code);
  WriteLn(r);
  Val('-0.25', r, code);
  WriteLn(r);
  Val('2', r, code);
  WriteLn(r);
  Val('0.001', r, code);
  WriteLn(r);
end.
