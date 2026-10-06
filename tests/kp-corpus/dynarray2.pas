program dynarray2;
{ Dynamic arrays of reals, strings, and nested arrays, the second slice. Real elements
  use the f64 read and write helpers. String elements are refcounted handles: a write
  retains the new value and releases the old, a shrink releases the dropped elements, and
  scope exit releases the rest (ASan/UBSan confirms no leak). A nested array is an element
  that is itself a dynamic array. Run under kprun it prints
  0.5 1 1.5 1 3 5 2 1 10 20 21 30 31 32. }
procedure emit(v: integer); external;
procedure emitf(x: real); external;

var
  r: array of real;
  s: array of string;
  m: array of array of integer;
  i, j: integer;
begin
  SetLength(r, 3);
  for i := 0 to High(r) do r[i] := (i + 1) / 2.0;
  for i := 0 to High(r) do emitf(r[i]);

  SetLength(s, 3);
  s[0] := 'a';
  s[1] := 'bcd';
  s[2] := 'hello';
  for i := 0 to High(s) do emit(Length(s[i]));
  s[1] := 'xy';                { overwrite: release 'bcd', retain 'xy' }
  emit(Length(s[1]));
  SetLength(s, 1);             { shrink: release the dropped elements }
  emit(Length(s));

  SetLength(m, 3);
  for i := 0 to High(m) do
  begin
    SetLength(m[i], i + 1);
    for j := 0 to High(m[i]) do
      m[i][j] := (i + 1) * 10 + j;
  end;
  for i := 0 to High(m) do
    for j := 0 to High(m[i]) do
      emit(m[i][j]);
end.
