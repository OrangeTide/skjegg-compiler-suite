program t047_enum;

type
  Color = (Red, Green, Blue, Yellow);
  Day = (Mon, Tue, Wed, Thu, Fri, Sat, Sun);

var
  c: Color;
  d: Day;

begin
  { Enum ordinal values — enum consts are i32 }
  writeln(Red);
  writeln(Green);
  writeln(Blue);
  writeln(Yellow);

  { Assignment and comparison }
  c := Green;
  if c = Green then
    writeln('green ok');

  c := Blue;
  if c <> Red then
    writeln('not red ok');

  { Write enum variable }
  writeln(c);

  { Day enum }
  d := Fri;
  writeln(d);

  { Compare enum values }
  if Sat > Fri then
    writeln('sat > fri');

  { Const expr with enum }
  writeln(Mon + 1);
end.
