{ Managed string members inside records and arrays: a record copy shares the
  string field by reference, reassigning one copy must not disturb the other,
  a record passed by value keeps its own reference, and string members of
  record and array locals are released on scope exit (the leak gate checks it). }
program RecordStringField;
type
  TRec = record
    name: string;
    age: integer;
  end;
  TList = array[1..3] of string;
var
  a, b: TRec;
  i: integer;

procedure show(r: TRec);
var
  names: TList;
begin
  { a local array of strings, built from heap concatenations and released
    when show returns }
  names[1] := r.name + '!';
  names[2] := r.name + '?';
  names[3] := r.name;
  writeln(names[1]);
  writeln(names[2]);
  writeln(r.name, ' ', r.age);
end;

begin
  a.name := 'ali';
  a.name := a.name + 'ce';   { a.name is now a heap string 'alice' }
  a.age := 30;

  b := a;                    { record copy: b.name shares a.name }
  b.name := 'bob';           { reassign b's field: a.name must stay 'alice' }
  b.age := 25;

  writeln(a.name);           { alice }
  writeln(b.name);           { bob }

  show(a);                   { record value parameter with a string field }
  show(b);

  for i := 1 to 2 do begin
    a.name := a.name + '.';  { self-append through a record field }
  end;
  writeln(a.name);           { alice.. }
end.
