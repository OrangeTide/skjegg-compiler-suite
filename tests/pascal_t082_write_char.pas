program t082_write_char;
var
  ch: char;
begin
  write(chr(72));
  write(chr(101));
  write(chr(108));
  write(chr(108));
  write(chr(111));
  writeln;
  ch := 'A';
  write(ch);
  writeln;
  write(chr(48 + 3));
  writeln;
end.
