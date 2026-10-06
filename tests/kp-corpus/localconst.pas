program localconst;
{ Local const sections inside routines, for scalar, typed-scalar, and string constants, plus
  a global string constant. A string constant materializes like a string literal, so Length,
  Ord, and character indexing work on it (as kpcom's EscapeStr uses its Hex constant). Run
  under kprun it prints 106 2 72 48 97. }
procedure emit(v: integer); external;

const
  greeting = 'Hi';

function scaled(n: integer): integer;
const
  base = 100;
  scale: integer = 3;
begin
  scaled := n * scale + base;
end;

function hexdigit(n: integer): integer;
const
  hex = '0123456789abcdef';
begin
  hexdigit := Ord(hex[n + 1]);   { 1-based: n=0 is '0', n=10 is 'a' }
end;

begin
  emit(scaled(2));          { 2*3 + 100 = 106 }
  emit(Length(greeting));   { 2 }
  emit(Ord(greeting[1]));   { 72, 'H' }
  emit(hexdigit(0));        { 48, '0' }
  emit(hexdigit(10));       { 97, 'a' }
end.
