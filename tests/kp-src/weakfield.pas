{ A weak interface field is accepted and stored (Slice 2 surface: parse + sema).
  The field is a non-retaining reference; here it is left nil, so the program just
  confirms a record carrying a weak field compiles and runs. }
program weakfield;
type
  INode = interface
    id: function: integer;
  end;
  TNode = record
    weak parent: INode;
    value: integer;
  end;
var
  n: TNode;
begin
  n.value := 7;
  writeln(n.value);
end.
