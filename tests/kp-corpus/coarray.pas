program coarray;
{ Coroutines stored in an array and resumed through the array index. The scheduler walks
  the array twice, resuming a chosen element each tick, then drops every coroutine
  mid-flight. Run under kprun it prints 11 21 31 12 22 32. }
procedure emit(v: integer); external;

procedure worker(id, steps: integer);
var i: integer;
begin
  for i := 1 to steps do
    yield id * 10 + i;
end;

var
  q: array[0..2] of process;
  pass, i: integer;
begin
  q[0] := start worker(1, 5);
  q[1] := start worker(2, 5);
  q[2] := start worker(3, 5);
  for pass := 1 to 2 do
    for i := 0 to 2 do
      emit(q[i](0));       { resume the coroutine held in element i }
  for i := 0 to 2 do
    release(q[i]);         { drop each one mid-flight }
end.
