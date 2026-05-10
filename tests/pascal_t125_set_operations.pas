{ Test set operations }
program TestSetOps;
var
  s1, s2, s3: set of byte;
  i: integer;
begin
  s1 := [1, 3, 5, 7, 9];
  s2 := [2, 4, 6, 8, 10];

  { membership }
  if 3 in s1 then writeln('3 in s1');
  if not (4 in s1) then writeln('4 not in s1');

  { union }
  s3 := s1 + s2;
  if (1 in s3) and (2 in s3) then writeln('union ok');

  { intersection }
  s3 := s1 * [1, 2, 3];
  if (1 in s3) and (3 in s3) and not (5 in s3) then
    writeln('intersection ok');

  { difference }
  s3 := s1 - [1, 3];
  if not (1 in s3) and (5 in s3) then
    writeln('difference ok');

  { empty set }
  s3 := [];
  if not (1 in s3) then writeln('empty ok');

  { range in set }
  s3 := [10..20];
  if (10 in s3) and (15 in s3) and (20 in s3) and not (9 in s3) then
    writeln('range ok');
end.
