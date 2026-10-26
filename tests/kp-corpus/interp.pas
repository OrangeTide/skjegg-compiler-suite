program interptest;
{ Double-quoted strings interpolate dollar-brace holes; single-quoted strings
  stay plain. A hole renders like write: string and char as-is, integers and
  int64 as their digits, bool and enum as their ordinal number. A doubled dollar
  escapes the hole marker, and a doubled quote is a literal quote. Prints the
  lines in interp.expected. }
type
  Color = (Red, Green, Blue);
var
  n: integer;
  big: int64;
  c: char;
  s: string;
  b: boolean;
  col: Color;
begin
  n := 42;
  big := 10000000000;
  c := 'Q';
  s := 'world';
  b := true;
  col := Green;
  WriteLn("hello ${s}!");
  WriteLn("n=${n} big=${big}");
  WriteLn("char=${c}");
  WriteLn("bool=${b} enum=${col}");
  WriteLn("sum=${n + 8}");
  WriteLn("literal $${not a hole} and $x");
  WriteLn("quote=""q""");
  WriteLn('plain ${notahole} stays');
  WriteLn("");
  WriteLn("end");
end.
