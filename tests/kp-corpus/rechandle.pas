program rechandle;
{ Records with a handle (string) field, the aggregate handle-field slice (3b). A
  whole-record copy retains the source's handle field, so overwriting the source does not
  free the value the copy still holds; a record local's handle field is released at scope
  exit. Run under kprun it prints 5 4 5 8. }
procedure emit(v: integer); external;

type
  R = record s: string; n: integer end;

procedure use(k: integer);
var
  r: R;
begin
  r.s := 'abcd';               { owned; released when use returns }
  r.n := k;
  emit(Length(r.s) + r.n);
end;

var
  a, b: R;
begin
  a.s := 'hello';
  b := a;                      { copy retains a.s, so b.s survives the next line }
  a.s := 'world';              { releases the old a.s; b.s still holds the string }
  emit(Length(b.s));           { 5 }
  use(0);                      { 4 }
  emit(Length(a.s));           { 5 }
  use(4);                      { 8 }
end.
