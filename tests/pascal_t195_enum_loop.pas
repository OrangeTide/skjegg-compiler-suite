{ Test looping over enum range }
program TestEnumLoop;
type
  Color = (Red, Green, Blue, Yellow, Cyan);
var
  c: Color;
begin
  for c := Red to Cyan do
    writeln(ord(c));
end.
