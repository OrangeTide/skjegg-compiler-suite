{ Test GCD and LCM algorithms }
program TestGcdLcm;
var
  a, b: integer;

function gcd(a, b: integer): integer;
begin
  while b <> 0 do begin
    a := a mod b;
    if a = 0 then begin
      gcd := b;
      exit;
    end;
    b := b mod a;
  end;
  gcd := a;
end;

function lcm(a, b: integer): integer;
begin
  lcm := (a div gcd(a, b)) * b;
end;

begin
  writeln(gcd(48, 18));
  writeln(gcd(100, 75));
  writeln(gcd(17, 13));
  writeln(gcd(1000, 1));
  writeln(lcm(12, 18));
  writeln(lcm(7, 5));
end.
