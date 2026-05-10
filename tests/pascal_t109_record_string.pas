{ Test records containing string fields }
program TestRecordString;
type
  Person = record
    name: string[15];
    age: integer;
  end;
var
  p: Person;
begin
  p.name := 'Alice';
  p.age := 30;
  writeln(p.name);
  writeln(p.age);

  p.name := 'Bob';
  p.age := 25;
  writeln(p.name);
  writeln(p.age);
  writeln(length(p.name));
end.
