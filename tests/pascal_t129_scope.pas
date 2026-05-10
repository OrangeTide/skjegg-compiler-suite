{ Test variable scoping }
program TestScope;
var
  x: integer;

procedure inner;
var
  x: integer;
begin
  x := 99;
  writeln(x);
end;

procedure modify;
begin
  x := 42;
end;

begin
  x := 10;
  writeln(x);
  inner;
  writeln(x);
  modify;
  writeln(x);
end.
