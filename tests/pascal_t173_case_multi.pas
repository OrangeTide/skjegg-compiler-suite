{ Test case with multiple values per arm }
program TestCaseMulti;
var
  i: integer;
begin
  for i := 0 to 12 do begin
    case i of
      0: write('zero');
      1, 2, 3: write('low');
      4, 5, 6: write('mid');
      7, 8, 9: write('high');
      10, 11, 12: write('top');
    end;
    if i < 12 then write(' ');
  end;
  writeln;
end.
