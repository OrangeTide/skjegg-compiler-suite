{ Test case with range labels }
program TestCaseRange;
var
  i: integer;
begin
  for i := 0 to 12 do begin
    case i of
      0: write('Z');
      1..3: write('L');
      4..6: write('M');
      7..9: write('H');
      10: write('X');
    else
      write('?');
    end;
  end;
  writeln;
end.
