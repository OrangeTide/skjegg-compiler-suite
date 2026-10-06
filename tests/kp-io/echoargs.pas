program echoargs;
{ echoes its command-line arguments (the .args fixture) via ParamStr/ParamCount. }
var
  i: integer;
begin
  WriteLn('argc=', ParamCount);
  for i := 1 to ParamCount do
    WriteLn(ParamStr(i));
end.
