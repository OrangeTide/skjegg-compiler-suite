program t053_set_compare;

type
  SmallSet = set of integer;

var
  s1, s2: SmallSet;

begin
  { Equality }
  s1 := [1, 2, 3];
  s2 := [1, 2, 3];
  if s1 = s2 then writeln('eq') else writeln('ne');

  s2 := [1, 2, 4];
  if s1 = s2 then writeln('eq') else writeln('ne');

  { Inequality }
  if s1 <> s2 then writeln('ne') else writeln('eq');

  { Subset }
  s1 := [2, 3];
  s2 := [1, 2, 3, 4];
  if s1 <= s2 then writeln('subset') else writeln('not subset');

  s1 := [2, 5];
  if s1 <= s2 then writeln('subset') else writeln('not subset');

  { Superset }
  s1 := [1, 2, 3, 4];
  s2 := [2, 3];
  if s1 >= s2 then writeln('superset') else writeln('not superset');

  s2 := [2, 5];
  if s1 >= s2 then writeln('superset') else writeln('not superset');

  { Empty set comparisons }
  s1 := [];
  s2 := [1];
  if s1 <= s2 then writeln('empty subset') else writeln('not subset');
  if s1 = [] then writeln('is empty') else writeln('not empty');
end.
