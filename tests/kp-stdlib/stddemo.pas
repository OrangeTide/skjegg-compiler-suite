program stddemo;
{ Exercises the std/io stdlib units over the freestanding rv tier: std.random
  (reproducibility + range), std.time (the monotonic clock across a park), and
  io.console (the end-of-input latch).  The
  output is deterministic: it reports facts, never the nondeterministic values
  (a clock reading, a random draw), so the .expected is stable. }
uses std.random, std.time, io.console;
var
  i, a, b, v: integer;
  t0, t1: Int64;
  ok: boolean;
begin
  { a pinned seed reproduces its sequence }
  random.SeedRandom(42);
  a := random.RandomBelow(1000000);
  random.SeedRandom(42);
  b := random.RandomBelow(1000000);
  if a = b then WriteLn('repro ok') else WriteLn('repro FAIL');

  { RandomBelow stays in range }
  random.SeedRandom(7);
  ok := true;
  for i := 1 to 1000 do begin
    v := random.RandomBelow(6);
    if (v < 0) or (v >= 6) then ok := false;
  end;
  if ok then WriteLn('range ok') else WriteLn('range FAIL');

  { the monotonic clock does not go backwards across a park }
  t0 := time.Now;
  time.Sleep(1);
  t1 := time.Now;
  if t1 >= t0 then WriteLn('time ok') else WriteLn('time FAIL');

  { with no stdin, the console is at end of input }
  if console.Eof then WriteLn('eof ok') else WriteLn('eof FAIL');
end.
