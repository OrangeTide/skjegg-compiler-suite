program failbinder;
{ A `fails e` error binder is scoped to the then-branch, so two sibling tests that reuse the
  name `e` each read their own error, and an outer variable `e` is not shadowed outside the
  branches. Before the scoping fix the second test read the first binder (FindLocal returns the
  first symbol by position). Run under kprun it prints 5 7 0 42, one per line. }
procedure emit(v: integer); external;

function parse(n: integer): integer fails;
begin
  if n < 0 then fail(0 - n);
  parse := n;
end;

var
  e: integer;
begin
  e := 42;
  if parse(-5) fails e then emit(errorcode(e)) else emit(0);
  if parse(-7) fails e then emit(errorcode(e)) else emit(0);
  if parse(9) fails e then emit(errorcode(e)) else emit(0);
  emit(e);
end.
