{ Test move builtin }
program TestMove;
var
  a, b: array[1..5] of integer;
  i: integer;
begin
  for i := 1 to 5 do
    a[i] := i * 10;
  fillchar(b, 5 * 4, 0);

  { copy a into b }
  move(a, b, 5 * 4);
  for i := 1 to 5 do begin
    if i > 1 then write(' ');
    write(b[i]);
  end;
  writeln;

  { partial copy }
  fillchar(b, 5 * 4, 0);
  move(a, b, 3 * 4);
  for i := 1 to 5 do begin
    if i > 1 then write(' ');
    write(b[i]);
  end;
  writeln;
end.
