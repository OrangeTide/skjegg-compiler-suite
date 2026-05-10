program t097_typed_const_set_subrange;

{ Typed constants initialized with subrange-based set types. }

type
  Digits = set of 0..9;
  Vowels = set of 'a'..'z';

const
  Odds: Digits = [1, 3, 5, 7, 9];
  V: Vowels = [97, 101, 105, 111, 117];

var
  i: integer;
begin
  for i := 0 to 9 do
    if i in Odds then write('1') else write('0');
  writeln;

  if 97 in V then write('1') else write('0');
  if 98 in V then write('1') else write('0');
  if 101 in V then write('1') else write('0');
  if 105 in V then write('1') else write('0');
  writeln;
end.
