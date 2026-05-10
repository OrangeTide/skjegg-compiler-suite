{ Test set of char operations }
program TestSetCharOps;
var
  vowels, consonants: set of char;
  ch: char;
  i: integer;
begin
  vowels := ['A', 'E', 'I', 'O', 'U',
             'a', 'e', 'i', 'o', 'u'];

  if 'a' in vowels then writeln('a is vowel');
  if not ('b' in vowels) then writeln('b not vowel');

  { count vowels in a string }
  i := 0;
  for ch := 'A' to 'Z' do
    if ch in vowels then
      i := i + 1;
  writeln(i);

  { intersection }
  consonants := ['A'..'Z'] - vowels;
  if 'B' in consonants then writeln('B is consonant');
  if not ('A' in consonants) then writeln('A not consonant');
end.
