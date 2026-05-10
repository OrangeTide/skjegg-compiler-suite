{ Test break in while loop }
program TestBreakWhile;
var
  i, sum: integer;
begin
  { find first multiple of 7 above 50 }
  i := 51;
  while i <= 100 do begin
    if i mod 7 = 0 then break;
    i := i + 1;
  end;
  writeln(i);

  { sum until exceeds 100 }
  sum := 0;
  i := 1;
  while true do begin
    sum := sum + i;
    if sum > 100 then break;
    i := i + 1;
  end;
  writeln(i);
  writeln(sum);
end.
