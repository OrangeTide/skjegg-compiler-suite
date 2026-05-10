{ Test nested case statements }
program TestNestedCase;
var
  x, y: integer;
begin
  for x := 1 to 3 do
    for y := 1 to 3 do begin
      case x of
        1: case y of
             1: write('A');
             2: write('B');
             3: write('C');
           end;
        2: case y of
             1: write('D');
             2: write('E');
             3: write('F');
           end;
        3: case y of
             1: write('G');
             2: write('H');
             3: write('I');
           end;
      end;
    end;
  writeln;
end.
