{ Test stack implementation using array }
program TestStackArray;
var
  stack: array[1..20] of integer;
  top: integer;

procedure push(val: integer);
begin
  top := top + 1;
  stack[top] := val;
end;

function pop: integer;
begin
  pop := stack[top];
  top := top - 1;
end;

function peek: integer;
begin
  peek := stack[top];
end;

var
  i: integer;
begin
  top := 0;
  push(10);
  push(20);
  push(30);
  writeln(peek);
  writeln(pop);
  writeln(pop);
  push(40);
  writeln(peek);
  writeln(pop);
  writeln(pop);
end.
