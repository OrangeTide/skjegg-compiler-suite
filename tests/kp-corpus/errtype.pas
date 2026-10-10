program errtype;

{ The opaque error type with its fail/errorcode builtins (int-code representation). An error is
  built only with fail() and read only with errorcode(); it never mixes with integer. See
  doc/languages/kobold/kp-errors.md. }

var
  e: error;
  n: integer;

function classify(x: integer): error;
begin
  if x < 0 then classify := fail(22)
  else classify := fail(0);
end;

begin
  e := fail(42);
  writeln(errorcode(e));            { 42 }
  e := fail(0);
  writeln(errorcode(e));            { 0 (the success sentinel) }

  { an error flows through a variable and a function result like any value }
  e := classify(-5);
  n := errorcode(e);
  writeln(n);                  { 22 }
  writeln(errorcode(classify(9))); { 0 }

  { errorcode() composes in an expression }
  if errorcode(fail(7)) = 7 then writeln(1) else writeln(0);   { 1 }
end.
