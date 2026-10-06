program charconcat;
{ Concatenation with char operands. A char is boxed into a one-character string, so
  char + char, string + char, and a chain of them all build a string. Run under kprun it
  prints 2 65 66 3 67 68 66. }
procedure emit(v: integer); external;
var
  s: string;
  c: char;
begin
  c := 'B';
  s := 'A' + c;                { a one-char string literal plus a char }
  emit(Length(s));            { 2 }
  emit(Ord(s[1]));            { 65 }
  emit(Ord(s[2]));            { 66 }
  s := Chr(67) + Chr(68) + c;  { char + char, then + char }
  emit(Length(s));            { 3 }
  emit(Ord(s[1]));            { 67 }
  emit(Ord(s[2]));            { 68 }
  emit(Ord(s[3]));            { 66 }
end.
