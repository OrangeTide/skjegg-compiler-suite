program t022_constparam;

procedure PrintDouble(const x: integer);
begin
  writeln(x * 2);
end;

procedure PrintSum(const a: integer; const b: integer);
begin
  writeln(a + b);
end;

procedure MixedParams(const c: integer; v: integer);
begin
  writeln(c + v);
end;

var
  n: integer;
begin
  PrintDouble(21);
  PrintSum(10, 25);
  n := 7;
  MixedParams(3, n);
end.
