program TypedConstSet;
type
  SmallSet = set of integer;
  CharSet = set of char;
const
  primes: SmallSet = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29];
  vowels: CharSet = ['a', 'e', 'i', 'o', 'u', 'A', 'E', 'I', 'O', 'U'];
  digits: CharSet = ['0'..'9'];
var
  i: integer;
  letters: string[10];
begin
  for i := 0 to 30 do
    if i in primes then write('y') else write('n');
  writeln;

  letters := 'abcdefghij';
  for i := 1 to 10 do
    if letters[i] in vowels then write('y') else write('n');
  writeln;

  letters := '0123456789';
  for i := 1 to 10 do
    if letters[i] in digits then write('y') else write('n');
  writeln
end.
