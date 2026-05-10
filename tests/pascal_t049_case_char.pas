program t049_case_char;

const
  Tab = 9;
  Space = 32;

var
  ch: char;
  i: integer;

procedure classify(c: integer);
begin
  case c of
    48..57: write('digit');
    65..90: write('upper');
    97..122: write('lower');
    32, 9, 10, 13: write('space');
  else
    write('other');
  end;
end;

begin
  classify(65);   { A -> upper }
  write(' ');
  classify(50);   { 2 -> digit }
  write(' ');
  classify(104);  { h -> lower }
  write(' ');
  classify(32);   { space }
  write(' ');
  classify(33);   { ! -> other }
  writeln;

  { Case with char constants }
  i := 43;
  case i of
    43: writeln('plus');
    45: writeln('minus');
    42: writeln('star');
    47: writeln('slash');
  end;
end.
