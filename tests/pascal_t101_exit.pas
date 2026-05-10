{ Test exit statement in procedures and functions }
program TestExit;

function FindFirst(n: integer): integer;
var i: integer;
begin
  FindFirst := -1;
  for i := 1 to 10 do begin
    if i * i >= n then begin
      FindFirst := i;
      exit;
    end;
  end;
end;

procedure PrintUntil(limit: integer);
var i: integer;
begin
  for i := 1 to 100 do begin
    if i > limit then
      exit;
    writeln(i);
  end;
end;

function WithValue(x: integer): integer;
begin
  if x > 0 then begin
    WithValue := x * 2;
    exit;
  end;
  WithValue := 0;
end;

begin
  writeln(FindFirst(10));
  writeln(FindFirst(50));
  PrintUntil(3);
  writeln(WithValue(5));
  writeln(WithValue(-1));
end.
