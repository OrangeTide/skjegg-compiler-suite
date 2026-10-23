{ Dynamic arrays (array of T) of scalar elements: SetLength, Length, High,
  indexing, growth preserving contents, shrink, and copy-on-write (a shared
  array is not disturbed when one side is resized). }
program DynArray;
type
  TIntArray = array of LongInt;
  TBigArray = array of Int64;
  TFlags = array of Boolean;
var
  a, b: TIntArray;
  big: TBigArray;
  flags: TFlags;
  i: integer;
begin
  SetLength(a, 5);
  for i := 0 to High(a) do
    a[i] := (i + 1) * 10;
  writeln(Length(a), ' ', High(a));
  for i := 0 to High(a) do
    writeln(a[i]);

  { grow preserves existing elements, zero-fills new ones }
  SetLength(a, 7);
  writeln(Length(a), ' ', a[0], ' ', a[4], ' ', a[5], ' ', a[6]);
  a[5] := 60;
  a[6] := 70;

  { assignment shares the descriptor }
  b := a;
  writeln(Length(b), ' ', b[6]);

  { copy-on-write: resizing the shared a leaves b intact }
  SetLength(a, 2);
  writeln(Length(a), ' ', a[0], ' ', a[1]);
  writeln(Length(b), ' ', b[6]);

  { Int64 elements (8-byte stride) }
  SetLength(big, 3);
  big[0] := 1000000000000;
  big[1] := big[0] * 2;
  big[2] := big[1] + 1;
  writeln(big[0], ' ', big[1], ' ', big[2]);

  { Boolean elements }
  SetLength(flags, 4);
  flags[1] := true;
  flags[3] := true;
  for i := 0 to High(flags) do
    if flags[i] then write('T') else write('F');
  writeln;
end.
