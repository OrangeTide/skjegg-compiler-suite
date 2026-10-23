{ Dynamic arrays of managed elements (array of string, array of record with a
  string field): element reference counting on grow, shrink, copy-on-write and
  by-value parameter passing.  A shrink releases the dropped elements; a shared
  resize (copy on write) retains the copied ones so the other side is intact. }
program MgdDynArray;
type
  TNames = array of string;
  TRec = record
    id: integer;
    name: string;
  end;
  TRecs = array of TRec;

procedure dump(a: TNames);
var
  i: integer;
begin
  for i := 0 to High(a) do
    writeln(a[i]);
end;

var
  names, other: TNames;
  recs: TRecs;
  i: integer;
begin
  SetLength(names, 3);
  names[0] := 'alpha';
  names[1] := 'beta';
  names[2] := 'gamma';
  dump(names);                      { by-value param: retain then release }

  other := names;                   { share the descriptor }
  writeln('copy1=', other[1]);

  { grow: names is shared, so it is copied; other keeps its three elements }
  SetLength(names, 4);
  names[3] := 'delta';
  writeln(Length(names), ' ', names[3]);
  writeln(Length(other), ' ', other[2]);

  { shrink: names is now private; gamma and delta are released }
  SetLength(names, 2);
  writeln(Length(names), ' ', names[0], ' ', names[1]);
  writeln(Length(other), ' ', other[0], ' ', other[2]);

  { array of record with a string field }
  SetLength(recs, 2);
  recs[0].id := 1;
  recs[0].name := 'one';
  recs[1].id := 2;
  recs[1].name := 'two';
  for i := 0 to High(recs) do
    writeln(recs[i].id, ' ', recs[i].name);

  { shrink releases recs[1].name }
  SetLength(recs, 1);
  writeln(Length(recs), ' ', recs[0].id, ' ', recs[0].name);
end.
