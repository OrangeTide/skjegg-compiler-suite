{ Test typed constant array as lookup table }
program TestConstArrayLookup;
const
  primes: array[1..10] of integer = (2, 3, 5, 7, 11, 13, 17, 19, 23, 29);
  squares: array[1..5] of integer = (1, 4, 9, 16, 25);
var
  i, sum: integer;
begin
  { Sum of first 10 primes }
  sum := 0;
  for i := 1 to 10 do
    sum := sum + primes[i];
  writeln(sum);

  { Sum of first 5 squares }
  sum := 0;
  for i := 1 to 5 do
    sum := sum + squares[i];
  writeln(sum);

  { 7th prime }
  writeln(primes[7]);
end.
