{ Test early exit from procedures and functions }
program TestExitEarly;
var
  count: integer;

procedure countUp(limit: integer);
var
  i: integer;
begin
  for i := 1 to 100 do begin
    if i > limit then
      exit;
    count := count + 1;
  end;
end;

function firstDiv(n, d: integer): integer;
var
  i: integer;
begin
  for i := d to n do begin
    if n mod i = 0 then begin
      firstDiv := i;
      exit;
    end;
  end;
  firstDiv := n;
end;

begin
  count := 0;
  countUp(5);
  writeln(count);

  count := 0;
  countUp(10);
  writeln(count);

  writeln(firstDiv(12, 2));
  writeln(firstDiv(13, 2));
  writeln(firstDiv(15, 4));
end.
