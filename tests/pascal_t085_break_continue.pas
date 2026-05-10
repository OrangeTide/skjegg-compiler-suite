program t085_break_continue;
var i: integer;
begin
  { break in while }
  i := 0;
  while true do begin
    i := i + 1;
    if i = 5 then break;
  end;
  writeln(i);

  { break in for }
  for i := 1 to 100 do begin
    if i = 3 then break;
  end;
  writeln(i);

  { break in repeat }
  i := 0;
  repeat
    i := i + 1;
    if i = 7 then break;
  until false;
  writeln(i);

  { continue in while }
  i := 0;
  while i < 10 do begin
    i := i + 1;
    if i mod 2 = 0 then continue;
    write(i);
  end;
  writeln;
end.
