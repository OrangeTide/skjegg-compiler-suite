{ Test case with ranges }
program TestCaseRange;
var
  ch: char;

function grade(score: integer): integer;
begin
  case score of
    90..100: grade := ord('A');
    80..89:  grade := ord('B');
    70..79:  grade := ord('C');
    60..69:  grade := ord('D');
  else
    grade := ord('F');
  end;
end;

begin
  ch := chr(grade(95)); write(ch);
  ch := chr(grade(85)); write(ch);
  ch := chr(grade(75)); write(ch);
  ch := chr(grade(65)); write(ch);
  ch := chr(grade(55)); write(ch);
  writeln;
end.
