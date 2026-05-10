{ Test record containing array }
program TestRecordArray;
type
  Stats = record
    values: array[1..4] of integer;
    count: integer;
  end;
var
  s: Stats;
  i, sum: integer;
begin
  s.count := 4;
  s.values[1] := 10;
  s.values[2] := 20;
  s.values[3] := 30;
  s.values[4] := 40;

  sum := 0;
  for i := 1 to s.count do
    sum := sum + s.values[i];
  writeln(sum);
  writeln(sum div s.count);
end.
