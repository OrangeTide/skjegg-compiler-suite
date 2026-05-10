{ Test string reversal }
program TestStringReverse;
var
  s: string[20];
  i, n: integer;
  ch: char;
begin
  s := 'Hello World';
  n := length(s);
  for i := 1 to n div 2 do begin
    ch := s[i];
    s[i] := s[n - i + 1];
    s[n - i + 1] := ch;
  end;
  writeln(s);

  s := 'ABCDE';
  n := length(s);
  for i := 1 to n div 2 do begin
    ch := s[i];
    s[i] := s[n - i + 1];
    s[n - i + 1] := ch;
  end;
  writeln(s);
end.
