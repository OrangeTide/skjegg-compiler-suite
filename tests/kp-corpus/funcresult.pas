program funcresult;
{ The function-result designator: a bare function name inside its own body is the result
  variable, for a function with parameters as well as a paramless one. It is readable (a
  concatenation that reads the result back) and assignable, and a paramless function reads
  its result by name while recursing with f(args). Strings are immutable, so the result is
  built with concatenation rather than element writes. Run under kprun it prints 5 104 108
  55 3 33. }
procedure emit(v: integer); external;

function lower(const s: string): string;
{ kpcom's own LowerStr shape: build the result by concatenating lowered characters. }
var
  i, c: integer;
begin
  lower := '';
  for i := 1 to Length(s) do
  begin
    c := Ord(s[i]);
    if (c >= 65) and (c <= 90) then c := c + 32;
    lower := lower + Chr(c);
  end;
end;

function fib(n: integer): integer;
{ A bare result read on the right, a recursive call with an argument list. }
begin
  if n < 2 then fib := n
  else fib := fib(n - 1) + fib(n - 2);
end;

function tag(const s: string): string;
{ Read the result back to build on it. }
begin
  tag := s;
  tag := tag + '!';
end;

var
  t: string;
begin
  t := lower('HeLLo');
  emit(Length(t));            { 5 }
  emit(Ord(t[1]));            { 104, 'h' }
  emit(Ord(t[3]));            { 108, 'l' }

  emit(fib(10));              { 55 }

  t := tag('hi');
  emit(Length(t));            { 3 }
  emit(Ord(t[3]));            { 33, '!' }
end.
