{ `@routine` takes a routine's procedural value. For a parameterless function this is the
  only way to take its address: its bare name calls it. For a procedure or a function with
  parameters, `@f` is the same value its bare name already yields. }
program procaddr;
type IntFn = function(x: integer): integer;
     NulFn = function: integer;
function answer: integer;
begin answer := 42; end;
function dbl(x: integer): integer;
begin dbl := x * 2; end;
var nf: NulFn;
    f:  IntFn;
begin
  nf := @answer;     { address, not a call }
  writeln(nf());     { 42 }
  writeln(answer);   { bare name calls: 42 }
  f := @dbl;         { @ on a function with parameters: same as bare dbl }
  writeln(f(21));    { 42 }
  f := dbl;
  writeln(f(5));     { 10 }
end.
