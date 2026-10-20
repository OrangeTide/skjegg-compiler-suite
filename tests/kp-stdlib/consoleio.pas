program consoleio;
{ Exercises io.console's typed read/write surface over the freestanding rv tier:
  a header through WriteStr/WriteLine (no stdin), then stdin echoed line by line
  with ReadLine until Eof, each line prefixed with WriteStr.  The input's final
  line has no trailing newline, so it also covers that ReadLine delivers it
  before Eof latches.  WriteChar takes a char value; a one-character string literal
  ('<') denotes a char, so it is passed directly, while a code goes through Chr. }
uses io.console;
begin
  console.WriteStr('lines:');
  console.WriteLine('');
  while not console.Eof do begin
    console.WriteStr('> ');
    console.WriteLine(console.ReadLine);
  end;
  console.WriteChar('<');                { a one-character string literal is a char }
  console.WriteStr('end>');
  console.WriteChar(Chr(10));            { newline }
end.
