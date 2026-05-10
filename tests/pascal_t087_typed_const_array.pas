program TypedConstArray;
const
  primes: array[1..5] of integer = (2, 3, 5, 7, 11);
  greet: array[0..4] of char = 'hello';
var
  i: integer;
begin
  for i := 1 to 5 do
    writeln(primes[i]);
  for i := 0 to 4 do
    write(greet[i]);
  writeln
end.
