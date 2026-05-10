program TestProc;
{ Test: parameterless procedure declaration and call. }

procedure Hello;
begin
  writeln('Hello from procedure')
end;

procedure Goodbye;
begin
  writeln('Goodbye from procedure')
end;

begin
  Hello;
  Goodbye;
  Hello
end.
