{ Test mutual recursion with forward declaration }
program TestMutualRecursion;

function isOdd(n: integer): boolean; forward;

function isEven(n: integer): boolean;
begin
  if n = 0 then
    isEven := true
  else
    isEven := isOdd(n - 1);
end;

function isOdd(n: integer): boolean;
begin
  if n = 0 then
    isOdd := false
  else
    isOdd := isEven(n - 1);
end;

var
  i: integer;
begin
  for i := 0 to 7 do begin
    if isEven(i) then
      write('E')
    else
      write('O');
  end;
  writeln;
end.
