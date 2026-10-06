program strchar;
{ Immutable strings, the character-indexing read. s[i] reads a byte (through kp_str_base and a
  bounds-checked LB); there is no s[i] := c and no SetLength(s), so a new string is built with
  concatenation. Assignment shares the value (retain, no copy). Ord and Chr convert between a
  character and its code. Run under kprun it prints 5 72 111 87 68 5 74 74. }
procedure emit(v: integer); external;

function upper(const s: string): string;
{ Build an uppercased copy by concatenating uppered characters; s is never touched. }
var
  r: string;
  i, c: integer;
begin
  r := '';
  for i := 1 to Length(s) do
  begin
    c := Ord(s[i]);
    if (c >= 97) and (c <= 122) then c := c - 32;
    r := r + Chr(c);
  end;
  upper := r;
end;

var
  a, b: string;
begin
  a := 'Hello';
  emit(Length(a));          { 5 }
  emit(Ord(a[1]));          { 72, 'H' }
  emit(Ord(a[5]));          { 111, 'o' }

  b := upper('world');
  emit(Ord(b[1]));          { 87, 'W' }
  emit(Ord(b[5]));          { 68, 'D' }
  emit(Length(b));          { 5 }

  a := Chr(74) + Copy(a, 2, Length(a) - 1);   { build 'Jello' anew, no in-place write }
  emit(Ord(a[1]));          { 74, 'J' }

  b := a;                   { share the value }
  emit(Ord(b[1]));          { 74, shares a's value }
end.
