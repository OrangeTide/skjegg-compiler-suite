{ Self-append (x := x + e): appends in place when x is uniquely owned, builds
  a fresh string when shared.  Covers a build loop, in-place self-doubling with
  an aliased operand (s := s + s), and copy-on-write so a shared value is not
  disturbed. }
program SelfAppend;
var
  s, t: string;
  i: integer;
begin
  { build a string by repeated self-append }
  s := '';
  for i := 1 to 5 do
    s := s + 'ab';
  writeln(s);
  writeln(length(s));

  { self-doubling with an aliased operand: s is uniquely owned and grows
    in place, appending its own bytes to itself }
  s := 'x';
  s := s + 'y';
  s := s + s;
  writeln(s);

  { copy-on-write: t shares s, then self-append on s must leave t intact }
  s := 'base';
  t := s;
  s := s + '!';
  writeln(s);
  writeln(t);
end.
