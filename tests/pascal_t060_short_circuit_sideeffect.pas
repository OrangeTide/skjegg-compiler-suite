program ShortCircuitSideEffect;
var counter: integer;

function Increment: boolean;
begin
  counter := counter + 1;
  Increment := true
end;

begin
  { 'and then' with false first: Increment should NOT be called }
  counter := 0;
  if false and then Increment then
    writeln('FAIL');
  writeln(counter);

  { 'and then' with true first: Increment should be called }
  counter := 0;
  if true and then Increment then
    writeln(counter);

  { 'or else' with true first: Increment should NOT be called }
  counter := 0;
  if true or else Increment then
    writeln(counter);

  { 'or else' with false first: Increment should be called }
  counter := 0;
  if false or else Increment then
    writeln(counter)
end.
