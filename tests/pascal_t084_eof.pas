program t084_eof;
var
  ch: char;
  count: integer;
begin
  count := 0;
  read(ch);
  while not eof do begin
    count := count + 1;
    read(ch);
  end;
  writeln(count);
end.
