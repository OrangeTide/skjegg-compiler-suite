{ Test local arrays in procedures }
program TestLocalArray;

procedure printReversed(n: integer);
var
  digits: array[1..10] of integer;
  count, i: integer;
begin
  count := 0;
  while n > 0 do begin
    count := count + 1;
    digits[count] := n mod 10;
    n := n div 10;
  end;
  for i := 1 to count do
    write(digits[i]);
  writeln;
end;

begin
  printReversed(12345);
  printReversed(9876);
  printReversed(100);
end.
