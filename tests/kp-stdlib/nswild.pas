program nswild;
{ The `uses ns.*` namespace wildcard binds every unit in a namespace under its own
  last-segment qualifier: std.* brings random. and time., io.* brings console. (only
  console is built in io/ for this test). Exercised on the freestanding rv tier: seed and
  draw from std.random, read the clock from std.time, and write through io.console, all
  reached by their wildcard-bound qualifiers. }
uses std.*, io.*;
var
  t: Int64;
begin
  random.SeedRandom(1);
  t := time.Now;
  console.WriteStr('wild');
  if (t >= 0) and (random.RandomBelow(10) >= 0) then
    console.WriteStr(' ok');
  console.WriteLine('');
end.
