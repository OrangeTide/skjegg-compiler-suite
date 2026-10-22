program realio;
{ Round-trip a real through the read seam: read(x) returns a real (the FCALL
  path on soft-float rv32), then write it and a derived value back.  This is the
  real-return marshalling the io.console ReadReal/std.random RandomReal units
  ride, proven on both freestanding tiers. }
var
  x: real;
begin
  read(x);
  WriteLn(x);
  WriteLn(x * 2.0);
  WriteLn(x + 1.0);
end.
