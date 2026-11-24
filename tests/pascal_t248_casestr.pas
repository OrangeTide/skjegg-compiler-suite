{ Case statement on a string selector: grouped labels and an else arm, matched
  through the string-compare runtime. }
program CaseStr;
procedure classify(s: string);
begin
  case s of
    'red', 'green', 'blue': writeln('colour');
    'cat', 'dog': writeln('animal')
  else
    writeln('unknown')
  end
end;
begin
  classify('red');
  classify('dog');
  classify('blue');
  classify('seven')
end.
