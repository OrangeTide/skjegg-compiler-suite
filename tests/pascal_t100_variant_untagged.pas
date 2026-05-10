program t100_variant_untagged;
type
  TValue = record
    vtype: integer;
    case integer of
      0: (ival: integer);
      1: (cval: char)
  end;
var
  v: TValue;
begin
  v.vtype := 0;
  v.ival := 42;
  writeln(v.vtype);
  writeln(v.ival);

  v.vtype := 1;
  v.cval := 'A';
  writeln(v.vtype);
  write(v.cval);
  writeln;
end.
