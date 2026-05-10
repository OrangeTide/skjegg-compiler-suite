{ Test extended enum usage }
program TestEnumExtended;
type
  Color = (Red, Green, Blue, Yellow, White);
var
  c: Color;
  i: integer;
begin
  c := Red;
  writeln(ord(c));

  c := Blue;
  writeln(ord(c));

  c := succ(c);
  writeln(ord(c));

  c := pred(c);
  writeln(ord(c));

  { enum in case }
  c := Green;
  case c of
    Red: writeln('red');
    Green: writeln('green');
    Blue: writeln('blue');
  else
    writeln('other');
  end;

  { enum comparison }
  if Red < Blue then
    writeln('red < blue');
  if White > Yellow then
    writeln('white > yellow');
end.
