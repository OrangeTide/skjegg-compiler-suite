{ Procedural types: a function type and a procedure type, a routine value taken
  with @, assigned to a variable and called through it, passed as a parameter
  and called inside the callee, and a procedure type with a var parameter that
  writes back through the code pointer.  A procedural value is a bare code
  pointer, so this is an indirect call. }
program ProcType;
type
  TBinOp = function(a, b: integer): integer;
  TMod = procedure(var x: integer);
var
  op: TBinOp;
  m: TMod;
  n: integer;

function add(a, b: integer): integer;
begin add := a + b end;

function mul(a, b: integer): integer;
begin mul := a * b end;

function apply(f: TBinOp; x, y: integer): integer;
begin apply := f(x, y) end;

procedure bump(var x: integer);
begin x := x + 5 end;

begin
  op := @add;
  writeln(op(3, 4));
  op := @mul;
  writeln(op(3, 4));
  writeln(apply(@add, 10, 20));
  writeln(apply(@mul, 10, 20));
  m := @bump;
  n := 100;
  m(n);
  writeln(n)
end.
