{ Test enum with set }
program TestEnumSet;
type
  Day = (Mon, Tue, Wed, Thu, Fri, Sat, Sun);
var
  weekday, weekend: set of Day;
  d: Day;
begin
  weekday := [Mon..Fri];
  weekend := [Sat, Sun];

  if Mon in weekday then writeln('Mon is weekday');
  if Sat in weekend then writeln('Sat is weekend');
  if not (Sun in weekday) then writeln('Sun not weekday');

  { count weekdays }
  d := Mon;
  while ord(d) <= ord(Fri) do begin
    if d in weekday then
      write(ord(d));
    d := succ(d);
  end;
  writeln;
end.
