{ Test typed constant arrays }
program TestArrayInit;
const
  primes: array[1..10] of integer = (2, 3, 5, 7, 11, 13, 17, 19, 23, 29);
var
  i, sum: integer;
begin
  sum := 0;
  for i := 1 to 10 do
    sum := sum + primes[i];
  writeln(sum);

  for i := 1 to 10 do begin
    if i > 1 then write(' ');
    write(primes[i]);
  end;
  writeln;
end.
