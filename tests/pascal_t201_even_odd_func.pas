{ Test even/odd detection functions }
program TestEvenOdd;

function isEven(n: integer): boolean;
begin
  isEven := (n mod 2 = 0);
end;

function isOdd(n: integer): boolean;
begin
  isOdd := (n mod 2 <> 0);
end;

var
  i: integer;
begin
  for i := 0 to 5 do begin
    if isEven(i) then
      write('E')
    else
      write('O');
  end;
  writeln;

  for i := -3 to 3 do begin
    if isOdd(i) then
      write('1')
    else
      write('0');
  end;
  writeln;
end.
