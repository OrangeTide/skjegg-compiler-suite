{ Test swap, lo, hi builtins }
program TestSwap;
var
  w: word;
begin
  w := $1234;
  writeln(lo(w));
  writeln(hi(w));
  w := swap(w);
  writeln(lo(w));
  writeln(hi(w));
  writeln(swap($0100));
  writeln(swap($00FF));
end.
