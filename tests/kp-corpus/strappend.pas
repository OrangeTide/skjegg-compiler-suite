program strappend;
{ The self-append idiom x := x + e grows a string in place when it is uniquely owned and
  copies when it is shared, so a build-a-string loop stays correct and keeps value
  semantics. Covers a char-append loop, a string append, an alias that must not grow with
  its source, and a self-double; then a left-leaning chain (x := x + a + b + c appends each
  in order) and a chain whose later operand reads the target, which falls back to a plain
  concat but stays correct. Run under kprun it prints
  5 65 69 7 121 7 8 33 16 6 97 102 5 45 81. }
procedure emit(v: integer); external;
var
  s, t: string;
  i: integer;
begin
  s := '';
  for i := 0 to 4 do
    s := s + Chr(65 + i);        { append a char each step: "ABCDE" }
  emit(Length(s));               { 5 }
  emit(Ord(s[1]));               { 65, 'A' }
  emit(Ord(s[5]));               { 69, 'E' }
  s := s + 'xy';                 { append a string: "ABCDExy" }
  emit(Length(s));               { 7 }
  emit(Ord(s[7]));               { 121, 'y' }
  t := s;                        { alias: t and s now share one value }
  s := s + '!';                  { s is shared, so this copies; t is untouched }
  emit(Length(t));               { 7, the alias did not grow }
  emit(Length(s));               { 8, "ABCDExy!" }
  emit(Ord(s[8]));               { 33, '!' }
  s := s + s;                    { self-double to length 16 }
  emit(Length(s));               { 16 }
  s := '';
  s := s + 'ab' + 'cd' + 'ef';   { a chain: each operand appended in order, "abcdef" }
  emit(Length(s));               { 6 }
  emit(Ord(s[1]));               { 97, 'a' }
  emit(Ord(s[6]));               { 102, 'f' }
  s := 'PQ';
  s := s + '-' + s;              { later operand reads s: falls back to a plain concat "PQ-PQ" }
  emit(Length(s));               { 5 }
  emit(Ord(s[3]));               { 45, '-' }
  emit(Ord(s[5]));               { 81, 'Q' }
end.
