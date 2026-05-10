program t095_set_char_subrange;

{ Set of char subrange: set of 'a'..'z' }

type
  LowerSet = set of 'a'..'z';
  UpperSet = set of 'A'..'Z';

var
  low: LowerSet;
  up:  UpperSet;

begin
  low := ['a', 'e', 'i', 'o', 'u'];
  if 'a' in low then write('1') else write('0');
  if 'b' in low then write('1') else write('0');
  if 'e' in low then write('1') else write('0');
  if 'z' in low then write('1') else write('0');
  writeln;

  up := ['A'..'E', 'Z'];
  if 'A' in up then write('1') else write('0');
  if 'C' in up then write('1') else write('0');
  if 'F' in up then write('1') else write('0');
  if 'Z' in up then write('1') else write('0');
  writeln;

  { Difference across subrange-bound sets }
  low := ['a'..'f'] - ['b', 'd'];
  if 'a' in low then write('1') else write('0');
  if 'b' in low then write('1') else write('0');
  if 'c' in low then write('1') else write('0');
  if 'd' in low then write('1') else write('0');
  if 'e' in low then write('1') else write('0');
  writeln;
end.
