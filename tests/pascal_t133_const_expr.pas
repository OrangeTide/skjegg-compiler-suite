{ Test constant expressions }
program TestConstExpr;
const
  MaxSize = 100;
  Half = MaxSize div 2;
  Pi100 = 314;
  Greeting = 'Hello';
  Yes = true;
  No = false;
  Star = '*';
begin
  writeln(MaxSize);
  writeln(Half);
  writeln(Pi100);
  writeln(Greeting);
  if Yes then writeln('yes');
  if not No then writeln('not no');
  writeln(Star);
end.
