{ Test running average computation }
program TestRunningAvg;
var
  a: array[1..6] of integer;
  i, sum: integer;
begin
  a[1] := 10; a[2] := 20; a[3] := 30;
  a[4] := 40; a[5] := 50; a[6] := 60;

  sum := 0;
  for i := 1 to 6 do begin
    sum := sum + a[i];
    writeln(sum div i);
  end;
end.
