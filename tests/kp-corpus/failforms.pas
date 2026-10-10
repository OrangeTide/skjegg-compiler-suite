program failforms;

{ The call-site forms of failable routines (doc/languages/kobold/kp-errors.md): `or else` recovery, the `fails`
  test with and without a binder, the `or exit|continue|fail` diverts, and `or else ignore`.
  parse doubles a non-negative number and fails with code 99 on a negative one. }

function parse(n: integer): integer fails;
begin
  if n < 0 then fail(99);
  parse := n * 2;
end;

function load(n: integer): integer fails;
begin
  load := parse(n) or fail;      { forward the inner failure unchanged }
end;

procedure note(n: integer) fails;
begin
  if n = 0 then fail(5);
end;

var
  i, sum: integer;

begin
  { or else: the value on success, the fallback on failure }
  writeln(parse(3) or else -1);        { 6 }
  writeln(parse(-3) or else -1);       { -1 }

  { fails test, with and without a binder }
  if parse(-7) fails e then writeln(errorcode(e)) else writeln(0);   { 99 }
  if parse(7) fails then writeln(-1) else writeln(1);                { 1 }

  { or fail through load, then recovered with or else }
  writeln(load(4) or else -1);         { 8 }
  writeln(load(-4) or else -2);        { -2 }

  { or continue: a diverting failure skips the rest of the iteration }
  sum := 0;
  for i := 3 downto -3 do
    sum := sum + (parse(i) or continue);
  writeln(sum);                        { 6+4+2+0 = 12; the negatives continue }

  { or else ignore: drop a failable procedure's error }
  note(0) or else ignore;
  writeln(42);                         { 42 }

  { or exit: leave main on failure, so the line after never prints }
  i := parse(-1) or exit;
  writeln(999);
end.
