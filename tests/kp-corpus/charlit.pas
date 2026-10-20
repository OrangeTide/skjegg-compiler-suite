program charlit;
{ A one-character string literal denotes a char where a char is expected: in a
  comparison, an assignment to a char, and a case label. Elsewhere it stays a
  string, and when passed as a char argument. Comparisons are tested through
  control flow, not as integer values.
  Run under kprun it prints 65 0 1 1 66 2 0 1 9 1. }
procedure emit(v: integer); external;

function classify(c: char): integer;
begin
  case c of
    'a'..'z': classify := 1;
    '0'..'9': classify := 2;
    ' ':      classify := 0;
  else
    classify := 9;
  end;
end;

var
  c: char;

begin
  c := 'A';                                  { char literal into a char }
  if c = 'A' then emit(Ord(c)) else emit(0); { 65 }
  if c = 'B' then emit(1) else emit(0);      { 0 }
  if (c >= 'A') and (c <= 'Z') then emit(1) else emit(0);  { 1, range compare }
  if c <> ' ' then emit(1) else emit(0);     { 1 }
  c := 'B';
  emit(Ord(c));                              { 66 }
  c := '5'; emit(classify(c));               { 2, case range }
  c := ' '; emit(classify(c));               { 0, single label }
  c := 'm'; emit(classify(c));               { 1 }
  emit(classify('A'));                        { 9, a char literal passed as a char argument }
  c := #9;
  if c = #9 then emit(1) else emit(0);       { 1, a control-char literal }
end.
