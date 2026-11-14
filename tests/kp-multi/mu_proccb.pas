program mu_proccb;
uses cbunit as _;
function dbl(x: integer): integer;
begin dbl := x * 2; end;
function inc1(x: integer): integer;
begin inc1 := x + 1; end;
var f: Op;              { name the imported proc type }
begin
  writeln(Apply(@dbl, 21));    { 42 }
  writeln(Twice(@inc1, 10));   { 12 }
  f := @dbl;                   { assign a routine value to an imported-proc-typed var }
  writeln(Apply(f, 5));        { 10 }
end.
