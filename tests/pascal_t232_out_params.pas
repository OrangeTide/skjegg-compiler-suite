{ `out` parameters (P5 stage A): a by-reference parameter the callee assigns.
  Treated like `var` (the compiler does not enforce the write-only rule), across
  a scalar, a string, and a dynamic array. }
program OutParams;
type
  TIntArray = array of LongInt;

procedure scan(n: LongInt; out value: LongInt);
begin
  value := n * n;
end;

function readWhole(const src: string; out data: string): Boolean;
begin
  data := 'got:' + src;
  readWhole := true;
end;

procedure fill(count: LongInt; out a: TIntArray);
var
  i: LongInt;
begin
  SetLength(a, count);
  for i := 0 to count - 1 do
    a[i] := i * 10;
end;

var
  v: LongInt;
  s: string;
  arr: TIntArray;
  ok: Boolean;
begin
  scan(6, v);
  writeln(v);
  ok := readWhole('hi', s);
  writeln(ord(ok), ' ', s);
  fill(3, arr);
  writeln(High(arr), ' ', arr[0], ' ', arr[1], ' ', arr[2]);
end.
