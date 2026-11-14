{ A parameterless function whose result is itself a procedure type. Used bare it is
  called (yielding the proc value); the returned code pointer is then called indirectly.
  Regression: lowering once took the address of such a function instead of calling it. }
program procret;
type IntFn = function(x: integer): integer;
function dbl(x: integer): integer;
begin dbl := x * 2; end;
function incr(x: integer): integer;
begin incr := x + 1; end;
function pick(which: integer): IntFn;
begin
  if which = 0 then pick := dbl else pick := incr;
end;
function first: IntFn;
begin first := dbl; end;
var f: IntFn;
begin
  f := first;        { bare parameterless function: a call, not its address }
  writeln(f(20));    { 40 }
  f := pick(0);
  writeln(f(20));    { 40 }
  f := pick(1);
  writeln(f(20));    { 21 }
end.
