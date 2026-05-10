program t048_case_basic;

var
  x: integer;
  i: integer;

begin
  { Basic case with single labels }
  x := 2;
  case x of
    1: writeln('one');
    2: writeln('two');
    3: writeln('three');
  end;

  { Case with else }
  x := 5;
  case x of
    1: writeln('one');
    2: writeln('two');
  else
    writeln('other');
  end;

  { Case with multiple labels }
  x := 3;
  case x of
    1, 3, 5: writeln('odd');
    2, 4, 6: writeln('even');
  end;

  { Case with ranges }
  x := 15;
  case x of
    1..10: writeln('1-10');
    11..20: writeln('11-20');
    21..30: writeln('21-30');
  else
    writeln('out of range');
  end;

  { Case with mixed labels and ranges }
  for i := 0 to 6 do begin
    case i of
      0: write('zero');
      1, 2: write('low');
      3..5: write('mid');
    else
      write('high');
    end;
    write(' ');
  end;
  writeln;
end.
