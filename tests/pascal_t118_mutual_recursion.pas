{ Test mutual recursion with forward declarations }
program TestMutualRecursion;

function IsEven(n: integer): integer; forward;
function IsOdd(n: integer): integer; forward;

function IsEven(n: integer): integer;
begin
  if n = 0 then
    IsEven := 1
  else
    IsEven := IsOdd(n - 1);
end;

function IsOdd(n: integer): integer;
begin
  if n = 0 then
    IsOdd := 0
  else
    IsOdd := IsEven(n - 1);
end;

var
  i: integer;
begin
  for i := 0 to 7 do
    write(IsEven(i));
  writeln;
  for i := 0 to 7 do
    write(IsOdd(i));
  writeln;
end.
