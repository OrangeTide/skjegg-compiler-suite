program strdemo;
{ Exercises the core.strings unit over the freestanding rv tier: case conversion, trimming,
  substring replacement, and the non-mutating string Delete/Insert (reached qualified as
  strings.* so they do not shadow the dynamic-array builtins).  Deterministic fact output. }
uses core.strings;
begin
  if strings.UpperCase('aBc9') = 'ABC9' then WriteLn('upper ok') else WriteLn('upper FAIL');
  if strings.LowerCase('aBc9') = 'abc9' then WriteLn('lower ok') else WriteLn('lower FAIL');
  if strings.Trim('  hi  ') = 'hi' then WriteLn('trim ok') else WriteLn('trim FAIL');
  if strings.StringReplace('a,b,c', ',', '-') = 'a-b-c' then WriteLn('replace ok') else WriteLn('replace FAIL');
  if strings.Delete('abcdef', 2, 3) = 'aef' then WriteLn('delete ok') else WriteLn('delete FAIL');
  if strings.Insert('XY', 'abc', 2) = 'aXYbc' then WriteLn('insert ok') else WriteLn('insert FAIL');
end.
