{ Test character classification }
program TestCharClassify;
var
  s: string[20];
  i, digits, uppers, lowers, others: integer;
begin
  s := 'Hello World 42!';
  digits := 0;
  uppers := 0;
  lowers := 0;
  others := 0;
  for i := 1 to length(s) do begin
    if (s[i] >= '0') and then (s[i] <= '9') then
      digits := digits + 1
    else if (s[i] >= 'A') and then (s[i] <= 'Z') then
      uppers := uppers + 1
    else if (s[i] >= 'a') and then (s[i] <= 'z') then
      lowers := lowers + 1
    else
      others := others + 1;
  end;
  writeln(digits);
  writeln(uppers);
  writeln(lowers);
  writeln(others);
end.
