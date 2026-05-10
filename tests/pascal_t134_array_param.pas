{ Test array parameters }
program TestArrayParam;
type
  IntArr = array[1..5] of integer;
var
  a: IntArr;
  i: integer;

procedure fill(var arr: IntArr; val: integer);
var
  i: integer;
begin
  for i := 1 to 5 do
    arr[i] := val + i;
end;

function sum(var arr: IntArr): integer;
var
  i, s: integer;
begin
  s := 0;
  for i := 1 to 5 do
    s := s + arr[i];
  sum := s;
end;

procedure double_arr(var arr: IntArr);
var
  i: integer;
begin
  for i := 1 to 5 do
    arr[i] := arr[i] * 2;
end;

begin
  fill(a, 10);
  for i := 1 to 5 do begin
    if i > 1 then write(' ');
    write(a[i]);
  end;
  writeln;
  writeln(sum(a));

  double_arr(a);
  for i := 1 to 5 do begin
    if i > 1 then write(' ');
    write(a[i]);
  end;
  writeln;
  writeln(sum(a));
end.
