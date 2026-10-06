program strleak;

{ A leak check for string processing. Work allocates and drops strings through every
  string-consuming operation (concatenation chains, pos, copy, length), so once it
  returns it should own nothing: its local is released on scope exit and every temporary
  it fed to a helper is released at the call site. Running it many times must leave the
  live guest-heap byte count where it started. A non-zero delta means an operation leaks
  an intermediate. }

function heapLive: integer; external;
procedure printInt(n: integer); external;

function makeArr(n: integer): array of integer;
begin
  SetLength(makeArr, n);         { an owned dynamic array, returned by value }
end;

procedure Work;
var
  s, t: string;
  n: integer;
begin
  s := 'a' + 'b' + 'c' + 'd';   { a concatenation chain: three intermediates }
  t := s + s;                    { both operands borrowed, result owned }
  n := pos('c', s);              { borrowed operand }
  n := pos('b' + 'c', t);        { an owned temporary fed to pos }
  s := copy(t, 1, 3);            { copy of a borrowed string, old s released }
  n := length(s + 'zz');         { an owned temporary fed to length }
  n := high(makeArr(4));         { high on an owned dynamic-array temporary }
  n := makeArr(4)[2];            { index an owned dynamic-array temporary }
  if n < 0 then printInt(n);     { keep n live so the compiler cannot drop the calls }
end;

var
  before, after, i: integer;
begin
  Work;                    { warm up any one-time allocation before the baseline }
  before := heapLive;
  for i := 1 to 1000 do
    Work;
  after := heapLive;
  printInt(after - before);   { 0 when nothing leaks }
end.
