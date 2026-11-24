{ Enumerated type with explicit ordinals: members without a value keep
  counting up from the previous one, members with `= N` jump to N, and the
  type's range spans the lowest and highest ordinal.  ord, low/high, succ,
  comparison and a for loop all see the explicit values. }
program EnumOrd;
type
  Day = (Mon, Tue = 10, Wed, Thu = 20, Fri);
var
  d: Day;
begin
  writeln(ord(Mon), ' ', ord(Tue), ' ', ord(Wed), ' ', ord(Thu), ' ', ord(Fri));
  writeln('low=', ord(low(Day)), ' high=', ord(high(Day)));
  d := Wed;
  writeln('d=', ord(d));
  d := succ(d);
  writeln('succ=', ord(d));
  if Fri > Thu then
    writeln('Fri>Thu');
  for d := Thu to Fri do
    writeln('loop ', ord(d))
end.
