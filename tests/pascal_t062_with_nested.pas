program WithNested;
type
  TInner = record
    a: integer;
  end;
  TOuter = record
    a: integer;
    inner: TInner;
  end;
var
  r: TOuter;
begin
  r.a := 1;
  r.inner.a := 2;
  with r do begin
    writeln(a);
  end;
  with r.inner do begin
    writeln(a);
    a := 99;
  end;
  writeln(r.inner.a)
end.
