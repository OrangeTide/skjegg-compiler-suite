{ Typed constant aggregates with string members (P5 stage C): a const array of
  strings indexed by an enum, a const array of records (string + enum fields),
  and a single record constant.  Each string slot is a static string object the
  image points at; a forward-referenced bound const resolves after the section. }
program TypedConst;
type
  TKind = (kA, kB, kC);
  TRec = record
    name: string;
    kind: TKind;
  end;
const
  Names: array[TKind] of string = ('alpha', 'beta', 'gamma');
  Count = 3;
  Table: array[1..Count] of TRec = (
    (name: 'and'; kind: kA),
    (name: 'or';  kind: kB),
    (name: 'not'; kind: kC)
  );
  Origin: TRec = (name: 'origin'; kind: kB);
var
  k: TKind;
  i: integer;
begin
  for k := kA to kC do
    writeln(Names[k]);
  for i := 1 to Count do
    writeln(Table[i].name, ' ', Ord(Table[i].kind));
  writeln(Origin.name, ' ', Ord(Origin.kind));
end.
