program forwarddecl;
{ A forward declaration lets a routine be called before its body appears. The
  later definition completes the same routine rather than declaring a second,
  so a call is not ambiguous. Mutual recursion is the classic case: isEven and
  isOdd each call the other. Run under kprun it prints 1 0 1 0 1. }
procedure emit(v: integer); external;

function isOdd(n: integer): integer; forward;

function isEven(n: integer): integer;
begin
  if n = 0 then isEven := 1
  else isEven := isOdd(n - 1);
end;

function isOdd(n: integer): integer;
begin
  if n = 0 then isOdd := 0
  else isOdd := isEven(n - 1);
end;

begin
  emit(isEven(0));   { 1 }
  emit(isEven(1));   { 0 }
  emit(isEven(4));   { 1 }
  emit(isOdd(2));    { 0 }
  emit(isOdd(7));    { 1 }
end.
