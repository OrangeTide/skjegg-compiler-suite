program t054_set_char;

type
  CharSet = set of char;

var
  digits, letters: CharSet;
  ch: char;

begin
  digits := ['0'..'9'];
  letters := ['A'..'Z', 'a'..'z'];

  { Test specific characters }
  if 'A' in letters then write('yes') else write('no');
  write(' ');
  if '5' in letters then write('yes') else write('no');
  writeln;

  { Test range boundaries }
  if '0' in digits then write('yes') else write('no');
  write(' ');
  if '9' in digits then write('yes') else write('no');
  write(' ');
  if '/' in digits then write('yes') else write('no');
  write(' ');
  if ':' in digits then write('yes') else write('no');
  writeln;

  { Test with char variable }
  ch := 'B';
  if ch in letters then write('yes') else write('no');
  write(' ');
  ch := '7';
  if ch in digits then write('yes') else write('no');
  write(' ');
  if ch in letters then write('yes') else write('no');
  writeln;
end.
