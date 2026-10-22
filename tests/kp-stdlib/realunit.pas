program realunit;
{ Exercises io.console's real surface (ReadReal/WriteReal) and std.random's
  real and secure draws over the freestanding rv tier.  ReadReal reads one real
  from stdin; WriteReal echoes it and a derived value, proving a real crosses
  the unit cdecl FFI on the rv32 soft-float tier.  The random checks report
  facts (reproducibility, range), never the nondeterministic values. }
uses io.console, std.random;
var
  x, r1, r2: real;
  n: Int64;
  s1, s2: integer;
begin
  x := console.ReadReal;
  console.WriteReal(x);
  console.WriteChar(Chr(10));
  console.WriteReal(x * 2.0);
  console.WriteChar(Chr(10));

  { ReadInt reads one signed decimal (a negative one, to prove the full 64-bit
    return is read, not a wrong half on the big-endian tier); writeln echoes it.
    WriteInt prints a known 32-bit Integer. }
  n := console.ReadInt;
  writeln(n);
  console.WriteInt(12345);
  console.WriteChar(Chr(10));

  { RandomReal reproduces under a pinned seed and stays in [0, 1) }
  random.SeedRandom(42);
  r1 := random.RandomReal;
  random.SeedRandom(42);
  r2 := random.RandomReal;
  if (r1 = r2) and (r1 >= 0.0) and (r1 < 1.0) then
    console.WriteLine('rreal ok')
  else
    console.WriteLine('rreal FAIL');

  { SecureRandom stays in range (not reproducible: it is entropy-seeded) }
  s1 := random.SecureRandom(100);
  s2 := random.SecureRandom(100);
  if (s1 >= 0) and (s1 < 100) and (s2 >= 0) and (s2 < 100) then
    console.WriteLine('secure ok')
  else
    console.WriteLine('secure FAIL');
end.
