{ Test building repeated strings }
program TestStringRepeat;
var
  s: string[30];
  i, len: integer;
begin
  { Build "AAAA" }
  s := '';
  len := 0;
  for i := 1 to 4 do begin
    len := len + 1;
    s[len] := 'A';
  end;
  s[0] := chr(len);
  writeln(s);

  { Build "12321" }
  s := '';
  len := 0;
  for i := 1 to 3 do begin
    len := len + 1;
    s[len] := chr(ord('0') + i);
  end;
  for i := 2 downto 1 do begin
    len := len + 1;
    s[len] := chr(ord('0') + i);
  end;
  s[0] := chr(len);
  writeln(s);

  writeln(length(s));
end.
