{ Test nested while loops }
program TestNestedWhile;
var
  i, j, sum: integer;
begin
  { Sum of products i*j for i=1..3, j=1..4 }
  sum := 0;
  i := 1;
  while i <= 3 do begin
    j := 1;
    while j <= 4 do begin
      sum := sum + i * j;
      j := j + 1;
    end;
    i := i + 1;
  end;
  writeln(sum);

  { Count pairs where i+j <= 5, i=1..4, j=1..4 }
  sum := 0;
  i := 1;
  while i <= 4 do begin
    j := 1;
    while j <= 4 do begin
      if i + j <= 5 then
        sum := sum + 1;
      j := j + 1;
    end;
    i := i + 1;
  end;
  writeln(sum);
end.
