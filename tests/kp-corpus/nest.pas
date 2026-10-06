program nest;
procedure emit(v: integer); external;
procedure outer(base: integer);
var total: integer;
  procedure add(delta: integer);
  begin
    total := total + delta + base;
  end;
  procedure twice;
  begin
    add(1);
    add(1);
  end;
begin
  total := 0;
  add(10);
  twice;
  emit(total);
end;
begin
  outer(100);
end.
