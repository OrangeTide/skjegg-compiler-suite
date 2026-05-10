{ Test record assignment (copy) }
program TestRecordAssign;
type
  Vec = record
    x, y, z: integer;
  end;
var
  a, b: Vec;
begin
  a.x := 10; a.y := 20; a.z := 30;
  b := a;
  writeln(b.x);
  writeln(b.y);
  writeln(b.z);

  { modify a, b should be unchanged }
  a.x := 99;
  writeln(a.x);
  writeln(b.x);
end.
