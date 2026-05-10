program t046_const_string;

const
  Greeting = 'Hello';
  Name = 'World';
  Separator = ', ';
  Num = 42;
  IsTrue = true;

begin
  writeln(Greeting);
  writeln(Name);
  write(Greeting);
  write(Separator);
  writeln(Name);

  { Integer constant in expression }
  writeln(Num + 8);

  { Boolean constant }
  if IsTrue then
    writeln('bool ok');
end.
