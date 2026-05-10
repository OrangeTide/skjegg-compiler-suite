program t079_set_char_ops;

type
  CharSet = set of char;

var
  vowels, consonants, letters, s1, s2: CharSet;
  ch: char;

begin
  vowels := ['A', 'E', 'I', 'O', 'U', 'a', 'e', 'i', 'o', 'u'];
  consonants := ['B'..'D', 'F'..'H', 'J'..'N', 'P'..'T', 'V'..'Z',
                 'b'..'d', 'f'..'h', 'j'..'n', 'p'..'t', 'v'..'z'];

  { Union }
  letters := vowels + consonants;
  if 'A' in letters then write('y') else write('n');
  if 'B' in letters then write('y') else write('n');
  if 'Z' in letters then write('y') else write('n');
  if '0' in letters then write('y') else write('n');
  writeln;

  { Intersection }
  s1 := ['A'..'Z'];
  s2 := vowels * s1;
  if 'A' in s2 then write('y') else write('n');
  if 'E' in s2 then write('y') else write('n');
  if 'a' in s2 then write('y') else write('n');
  if 'B' in s2 then write('y') else write('n');
  writeln;

  { Difference }
  s1 := ['A'..'Z'] - vowels;
  if 'B' in s1 then write('y') else write('n');
  if 'A' in s1 then write('y') else write('n');
  if 'Z' in s1 then write('y') else write('n');
  if 'E' in s1 then write('y') else write('n');
  writeln;

  { Equality }
  s1 := ['A', 'E', 'I', 'O', 'U', 'a', 'e', 'i', 'o', 'u'];
  if s1 = vowels then write('eq') else write('ne');
  write(' ');
  s2 := ['A'..'Z'];
  if s2 = vowels then write('eq') else write('ne');
  writeln;

  { Inequality }
  if s1 <> vowels then write('ne') else write('eq');
  write(' ');
  if s2 <> vowels then write('ne') else write('eq');
  writeln;

  { Subset <= }
  if vowels <= letters then write('y') else write('n');
  write(' ');
  if letters <= vowels then write('y') else write('n');
  writeln;

  { Superset >= }
  if letters >= vowels then write('y') else write('n');
  write(' ');
  if vowels >= letters then write('y') else write('n');
  writeln;

  { Empty set assignment and comparison }
  s1 := [];
  if s1 = [] then write('empty') else write('not');
  writeln;

  { Compound expression: (a + b) = (c + d) }
  s1 := vowels + consonants;
  s2 := ['A'..'Z', 'a'..'z'];
  if s1 = s2 then write('eq') else write('ne');
  writeln;
end.
