{ Test deeply nested record access }
program TestNestedRecord;
type
  Inner = record
    value: integer;
  end;
  Middle = record
    a, b: Inner;
  end;
  Outer = record
    left, right: Middle;
  end;
var
  o: Outer;
begin
  o.left.a.value := 1;
  o.left.b.value := 2;
  o.right.a.value := 3;
  o.right.b.value := 4;

  writeln(o.left.a.value);
  writeln(o.left.b.value);
  writeln(o.right.a.value);
  writeln(o.right.b.value);
  writeln(o.left.a.value + o.right.b.value);
end.
