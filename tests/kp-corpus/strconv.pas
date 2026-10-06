program strconv;
{ String query and conversion builtins, the second dynamic-string slice. Str formats an
  integer into a string; Val parses a string into an integer or a real and reports success
  in a code (0 for a full valid parse, else the 1-based position of the first bad character,
  as FPC's Val); Pos gives the 1-based position of a substring or 0; Copy takes a substring.
  Run under kprun it prints 4 52 123 0 3 255 0 1 3.5 0 150 4 0 3 101 108. }
procedure emit(v: integer); external;
procedure emitf(x: real); external;

var
  s, t: string;
  i, code: integer;
  r: real;
begin
  Str(4096, s);
  emit(Length(s));            { 4 }
  emit(Ord(s[1]));            { 52, the digit '4' }

  Val('123', i, code);
  emit(i); emit(code);        { 123 0 }
  Val('12x', i, code);
  emit(code);                 { 3, the position of 'x' }
  Val('$FF', i, code);
  emit(i); emit(code);        { 255 0, a hex literal }
  Val('', i, code);
  emit(code);                 { 1, nothing to parse }

  Val('3.5', r, code);
  emitf(r); emit(code);       { 3.5 0 }
  Val('1.5e2', r, code);
  emitf(r);                   { 150 }

  emit(Pos('lo', 'Hello'));   { 4 }
  emit(Pos('z', 'Hello'));    { 0 }

  t := Copy('Hello', 2, 3);   { 'ell' }
  emit(Length(t));            { 3 }
  emit(Ord(t[1]));            { 101, 'e' }
  emit(Ord(t[3]));            { 108, 'l' }
end.
