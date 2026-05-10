program t096_set_enum_subrange;

{ Set of enum subrange forms:
    set of Day(Mon..Fri)     named-subrange rooted in enum type
    set of Mon..Fri          subrange literal using enum constants   }

type
  Day      = (Sun, Mon, Tue, Wed, Thu, Fri, Sat);
  Weekday  = set of Day(Mon..Fri);
  Workdays = set of Mon..Fri;

var
  w: Weekday;
  x: Workdays;
  d: Day;

begin
  w := [Mon, Wed, Fri];
  x := [Mon..Fri] - [Wed];

  for d := Sun to Sat do begin
    if d in w then write('1') else write('0');
  end;
  writeln;

  for d := Sun to Sat do begin
    if d in x then write('1') else write('0');
  end;
  writeln;

  { Union of two enum-subrange sets with compatible bases }
  w := w + x;
  for d := Sun to Sat do begin
    if d in w then write('1') else write('0');
  end;
  writeln;
end.
