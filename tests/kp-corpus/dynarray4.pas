program dynarray4;
{ Dynamic arrays of records with a handle (string) field, the aggregate handle-field slice
  over the guest heap (3b). The array carries a runtime layout map of the element's handle
  offsets: a shrink releases the dropped elements' string fields through it, a whole-record
  write into an element retains the source's field and releases the element's old one, and
  the reclaim on teardown releases the survivors. Run under kprun it prints 5 9 7 9 7. }
procedure emit(v: integer); external;

type
  Item = record s: string; n: integer end;

var
  a: array of Item;
  it: Item;
  i: integer;
begin
  SetLength(a, 3);
  for i := 0 to High(a) do
  begin
    a[i].s := 'hello';         { string field of a record element }
    a[i].n := i + 1;
  end;
  emit(Length(a[0].s));        { 5 }

  it.s := 'worldwide';
  it.n := 7;
  a[0] := it;                  { whole-record write: retains it.s, frees the old a[0].s }
  emit(Length(a[0].s));        { 9 }
  emit(a[0].n);                { 7 }

  SetLength(a, 1);             { shrink: releases a[1].s and a[2].s }
  emit(Length(a[0].s));        { 9, the survivor is intact }
  emit(a[0].n);                { 7 }
end.
