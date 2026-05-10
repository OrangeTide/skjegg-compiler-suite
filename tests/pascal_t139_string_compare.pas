{ Test string comparisons }
program TestStringCompare;
var
  a, b: string[20];
begin
  a := 'Apple';
  b := 'Banana';

  if a < b then writeln('Apple < Banana');
  if b > a then writeln('Banana > Apple');

  a := 'Test';
  b := 'Test';
  if a = b then writeln('equal');
  if not (a <> b) then writeln('not different');

  { length matters }
  a := 'AB';
  b := 'ABC';
  if a < b then writeln('AB < ABC');

  { case sensitive }
  a := 'abc';
  b := 'ABC';
  if a > b then writeln('abc > ABC');
end.
