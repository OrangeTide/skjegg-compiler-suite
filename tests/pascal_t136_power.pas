{ Test iterative and recursive power }
program TestPower;

function power(base, exp: integer): integer;
var
  result: integer;
begin
  result := 1;
  while exp > 0 do begin
    if odd(exp) then
      result := result * base;
    base := base * base;
    exp := exp div 2;
  end;
  power := result;
end;

function power_rec(base, exp: integer): integer;
begin
  if exp = 0 then
    power_rec := 1
  else if odd(exp) then
    power_rec := base * power_rec(base, exp - 1)
  else
    power_rec := sqr(power_rec(base, exp div 2));
end;

begin
  writeln(power(2, 0));
  writeln(power(2, 10));
  writeln(power(3, 5));
  writeln(power(7, 4));
  writeln(power_rec(2, 10));
  writeln(power_rec(3, 5));
end.
