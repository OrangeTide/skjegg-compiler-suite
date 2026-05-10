program t052_set_basic;

type
  SmallSet = set of integer;

var
  s1, s2, s3: SmallSet;
  i: integer;

begin
  { Empty set }
  s1 := [];
  if s1 = [] then writeln('empty') else writeln('not empty');

  { Single element }
  s1 := [3];
  if 3 in s1 then write('yes') else write('no');
  write(' ');
  if 4 in s1 then write('yes') else write('no');
  writeln;

  { Multiple elements }
  s1 := [1, 3, 5];
  for i := 0 to 6 do begin
    if i in s1 then write('1') else write('0');
  end;
  writeln;

  { Range }
  s1 := [2..5];
  for i := 0 to 7 do begin
    if i in s1 then write('1') else write('0');
  end;
  writeln;

  { Mixed elements and ranges }
  s1 := [0, 3..5, 7];
  for i := 0 to 8 do begin
    if i in s1 then write('1') else write('0');
  end;
  writeln;

  { Union }
  s1 := [1, 2];
  s2 := [3, 4];
  s3 := s1 + s2;
  for i := 0 to 5 do begin
    if i in s3 then write('1') else write('0');
  end;
  writeln;

  { Intersection }
  s1 := [1, 2, 3];
  s2 := [2, 3, 4];
  s3 := s1 * s2;
  for i := 0 to 5 do begin
    if i in s3 then write('1') else write('0');
  end;
  writeln;

  { Difference }
  s1 := [1, 2, 3, 4];
  s2 := [2, 4];
  s3 := s1 - s2;
  for i := 0 to 5 do begin
    if i in s3 then write('1') else write('0');
  end;
  writeln;
end.
