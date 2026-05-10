{ Test sieve of Eratosthenes using sets }
program TestPrimeSieve;
var
  sieve: set of byte;
  i, j: integer;
  count: integer;
begin
  sieve := [2..100];
  i := 2;
  while i * i <= 100 do begin
    if i in sieve then begin
      j := i * i;
      while j <= 100 do begin
        sieve := sieve - [j];
        j := j + i;
      end;
    end;
    i := i + 1;
  end;

  { count primes }
  count := 0;
  for i := 2 to 100 do
    if i in sieve then
      count := count + 1;
  writeln(count);

  { print first few }
  j := 0;
  for i := 2 to 30 do
    if i in sieve then begin
      if j > 0 then write(' ');
      write(i);
      j := j + 1;
    end;
  writeln;
end.
