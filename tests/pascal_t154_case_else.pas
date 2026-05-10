{ Test case with else clause }
program TestCaseElse;
var
  grade: integer;
begin
  for grade := 0 to 5 do begin
    case grade of
      5: write('A');
      4: write('B');
      3: write('C');
      2: write('D');
      1: write('F');
    else
      write('?');
    end;
  end;
  writeln;

  { case with negative values }
  grade := -1;
  case grade of
    -1: writeln('minus one');
    0: writeln('zero');
    1: writeln('one');
  end;
end.
