program intrinsics;
{ The Inc / Dec / Exit intrinsics. Inc and Dec adjust an ordinal designator in place,
  with an optional step. Exit returns early, keeping the result assigned so far; Exit(v)
  sets the function result first, including a handle (string) result. Run under kprun it
  prints 6 10 9 6 3 8 7 3 3 2. }
procedure emit(v: integer); external;

function first_even(const a: array of integer): integer;
var
  i: integer;
begin
  first_even := -1;
  for i := 0 to High(a) do
    if a[i] mod 2 = 0 then begin
      first_even := a[i];
      Exit;                { early return, keeping the assigned result }
    end;
end;

function pick(b: boolean): integer;
begin
  if b then Exit(7);       { Exit with a value }
  pick := 3;
end;

function greet(b: boolean): string;
begin
  greet := 'no';
  if b then Exit('yes');   { Exit with a handle (string) result }
end;

var
  x, i, n: integer;
  d: array of integer;
  s: string;
begin
  x := 5;
  Inc(x);        emit(x);   { 6 }
  Inc(x, 4);     emit(x);   { 10 }
  Dec(x);        emit(x);   { 9 }
  Dec(x, 3);     emit(x);   { 6 }

  n := 0;
  for i := 1 to 100 do begin
    Inc(n);
    if n = 3 then Break;
  end;
  emit(n);                  { 3 }

  SetLength(d, 4);
  d[0] := 1; d[1] := 3; d[2] := 8; d[3] := 9;
  emit(first_even(d));      { 8 }
  emit(pick(True));         { 7 }
  emit(pick(False));        { 3 }

  s := greet(True);  emit(Length(s));   { 3, 'yes' }
  s := greet(False); emit(Length(s));   { 2, 'no' }
end.
