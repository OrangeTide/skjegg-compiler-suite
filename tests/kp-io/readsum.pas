program readsum;
{ reads two integers and a line of text from stdin (the .input fixture). }
var
  a, b: integer;
  name: string;
begin
  ReadLn(a, b);
  ReadLn(name);
  WriteLn('sum=', a + b);
  WriteLn('hello ', name);
end.
