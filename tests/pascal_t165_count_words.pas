{ Test word counting in a string }
program TestCountWords;
var
  s: string[40];
  i, count: integer;
  inWord: boolean;
begin
  s := 'The quick brown fox jumps';
  count := 0;
  inWord := false;
  for i := 1 to length(s) do begin
    if s[i] = ' ' then
      inWord := false
    else if not inWord then begin
      inWord := true;
      count := count + 1;
    end;
  end;
  writeln(count);

  { single word }
  s := 'Hello';
  count := 0;
  inWord := false;
  for i := 1 to length(s) do begin
    if s[i] = ' ' then
      inWord := false
    else if not inWord then begin
      inWord := true;
      count := count + 1;
    end;
  end;
  writeln(count);
end.
