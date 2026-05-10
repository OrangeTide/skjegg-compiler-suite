{ Test array summation patterns }
program TestArraySum;
var
  a: array[1..10] of integer;
  i, sum, product: integer;
begin
  for i := 1 to 10 do
    a[i] := i;

  { sum }
  sum := 0;
  for i := 1 to 10 do
    sum := sum + a[i];
  writeln(sum);

  { sum of squares }
  sum := 0;
  for i := 1 to 10 do
    sum := sum + a[i] * a[i];
  writeln(sum);

  { product of first 5 }
  product := 1;
  for i := 1 to 5 do
    product := product * a[i];
  writeln(product);

  { running sum }
  sum := 0;
  for i := 1 to 5 do begin
    sum := sum + a[i];
    write(sum);
    if i < 5 then write(' ');
  end;
  writeln;
end.
