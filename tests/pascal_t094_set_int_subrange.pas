program t094_set_int_subrange;

{ Set of integer subrange literal: set of Lo..Hi }

type
  Digits = set of 0..9;
  Teens  = set of 13..19;

var
  d: Digits;
  t: Teens;
  i: integer;

begin
  d := [1, 3, 5, 7, 9];
  for i := 0 to 9 do
    if i in d then write('1') else write('0');
  writeln;

  t := [13..15, 18];
  for i := 13 to 19 do
    if i in t then write('1') else write('0');
  writeln;

  { Union across same type }
  d := [0, 2, 4] + [5, 7];
  for i := 0 to 9 do
    if i in d then write('1') else write('0');
  writeln;
end.
